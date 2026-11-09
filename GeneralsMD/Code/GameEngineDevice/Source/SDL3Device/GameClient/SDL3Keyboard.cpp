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
** SDL3Keyboard.cpp
**
** SDL3-based keyboard implementation for Linux builds.
**
** TheSuperHackers @feature CnC_Generals_Linux 10/02/2026 Bender
** Replaces Win32DIKeyboard with SDL3 keyboard APIs for Linux.
*/

#ifndef _WIN32

// GeneralsX @bugfix BenderAI 13/02/2026 Fix include path (fighter19 pattern)
#include "SDL3Device/GameClient/SDL3Keyboard.h"
#include <cstdio>
#include <cstring>

/**
 * Constructor
 */
SDL3Keyboard::SDL3Keyboard(void)
	: Keyboard(),
	  m_nextFreeIndex(0),
	  m_nextGetIndex(0)
{
	// Initialize event buffer with SDL_EVENT_FIRST sentinel
	// GeneralsX @refactor felipebraz 16/02/2026 Fighter19 pattern
	memset(m_eventBuffer, 0, sizeof(m_eventBuffer));
	memset(m_layoutKey, 0, sizeof(m_layoutKey));
}

/**
 * Destructor
 */
SDL3Keyboard::~SDL3Keyboard(void)
{
	// destructor
}

/**
 * Initialize keyboard subsystem
 */
void SDL3Keyboard::init(void)
{
	// Call parent init
	Keyboard::init();
	
	// TODO: Phase 2 - Get SDL3 keyboard state array
	// m_SDLKeyState = SDL_GetKeyboardState(&m_NumKeys);
	
	// Clear event buffer - Fighter19 pattern
	// GeneralsX @refactor felipebraz 16/02/2026
	memset(m_eventBuffer, 0, sizeof(m_eventBuffer));
	m_nextFreeIndex = 0;
	m_nextGetIndex = 0;
	
	// init complete
}

/**
 * Reset keyboard to default state
 */
void SDL3Keyboard::reset(void)
{
	Keyboard::reset();
	
	// Clear event buffer - Fighter19 pattern
	// GeneralsX @refactor felipebraz 16/02/2026
	memset(m_eventBuffer, 0, sizeof(m_eventBuffer));
	m_nextFreeIndex = 0;
	m_nextGetIndex = 0;
}

/**
 * Update keyboard state (called per-frame)
 */
void SDL3Keyboard::update(void)
{
	// Call parent update (processes events from getKey())
	Keyboard::update();
}

/**
 * Get keyboard data structure
 * Called by game systems to query keyboard state
 */
KeyboardIO *SDL3Keyboard::getKeyboard(void)
{
	// For Phase 1.5, return nullptr - full implementation in Phase 2
	// TODO: Phase 2 - Return keyboard state structure
	return nullptr;
}

/**
 * Get Caps Lock state (Linux stub)
 * Returns 0 (Caps Lock not active) - Windows would query keyboard driver state
 * On Linux/SDL3, we don't track Caps Lock state yet
 * GeneralsX @build fbraz 12/02/2026 Bender
 */
Bool SDL3Keyboard::getCapsState(void)
{
	// TODO: Phase 2 - Query SDL3 keyboard modifiers for Caps Lock
	// For now, assume Caps Lock is not active
	return 0;
}

// GeneralsX @feature Claude 25/09/2026 The character the active layout puts on a key, unshifted (see
// Keyboard::getLayoutKey): on French AZERTY the key at US ';' types "m", and this says so where the
// engine's US key table cannot. 0 for a key that types no printable character.
static WideChar layoutCharOf(SDL_Scancode scancode)
{
	const SDL_Keycode k = SDL_GetKeyFromScancode(scancode, SDL_KMOD_NONE, true);
	return ((k >= 0x20 && k < 0x7F) || (k >= 0xA0 && k <= 0xFFFF)) ? (WideChar)k : 0;
}

// No fallback to the key table: a key the layout gives no printable character (a dead key) names no
// letter, rather than the US table's letter for that position (codex keys-b1-r7). Every key-down sets its
// entry in getKey before the key reaches the GUI, so the only caller - the drop-down letter keys, asking
// about the key-down it is handling - never meets a key not seen yet; one would name nothing too.
WideChar SDL3Keyboard::getLayoutKey(KeyDefType key)
{
	return ((Int)key > 0 && (Int)key < KEY_COUNT) ? m_layoutKey[key] : 0;
}

/**
 * Get next keyboard event (called by parent Keyboard::update())
 * Fighter19 pattern: check for SDL_EVENT_FIRST sentinel (means empty)
 */
void SDL3Keyboard::getKey(KeyboardIO *key)
{
	// GeneralsX @bugfix Claude 17/08/2026 An unmapped key must not end the frame's batch.
	//
	// KEY_NONE has two meanings that this function used to conflate. To Keyboard::updateKeys()
	// it is the END-OF-STREAM sentinel - its read loop is
	// `do { getKey(&m_keys[index]); } while (m_keys[index++].key != KEY_NONE);` - but
	// translateScanCodeToKeyVal() also returns KEY_NONE for any scancode it does not map.
	// So one press of an unmapped key truncated the whole frame's keyboard batch: everything
	// still queued behind it was deferred a frame, and the modifier post-pass at the bottom of
	// updateKeys() ran against a half-read batch.
	//
	// That is not hypothetical on macOS. Command (SDL_SCANCODE_LGUI / RGUI) has no KeyDefType
	// at all - KeyDefs.h has no KEY_LWIN - so every Cmd-Tab, Cmd-Space and Cmd-anything a Mac
	// user presses hit this path. Skipping unmapped events instead consumes them and keeps
	// reading, so KEY_NONE once again means only "no more keys".
	key->key = KEY_NONE;
	key->status = KeyboardIO::STATUS_UNUSED;
	key->state = 0;

	// Buffer is empty when the slot still holds the SDL_EVENT_FIRST (0) sentinel.
	// GeneralsX @refactor felipebraz 16/02/2026 Fighter19 pattern: sentinel check
	while (m_eventBuffer[m_nextGetIndex].type != SDL_EVENT_FIRST) {

		const SDL_Event &event = m_eventBuffer[m_nextGetIndex];
		const Bool isKeyEvent = (event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_KEY_UP);

		KeyDefType keyDef = KEY_NONE;
		UnsignedShort state = 0;

		if (isKeyEvent) {
			// Translate SDL3 scancode to game KeyDefType
			keyDef = translateScanCodeToKeyVal(event.key.scancode);
			if (event.key.down && keyDef != KEY_NONE) {
				m_layoutKey[keyDef] = layoutCharOf(event.key.scancode);
			}
			state = event.key.down ? KEY_STATE_DOWN : KEY_STATE_UP;
		}

		// Consume the slot: mark empty (sentinel) and advance the read position.
		m_eventBuffer[m_nextGetIndex].type = SDL_EVENT_FIRST;
		m_nextGetIndex = (m_nextGetIndex + 1) % MAX_SDL3_KEY_EVENTS;

		if (keyDef == KEY_NONE) {
			// Unmapped key, or a non-keyboard event that reached the buffer. Drop it and
			// keep reading rather than reporting end-of-stream.
			continue;
		}

		key->key = keyDef;
		key->state = state;
		return;
	}
}

/**
 * Add raw SDL_Event to keyboard event buffer
 * Fighter19 pattern: unified method that accepts any SDL_Event
 * Called by SDL3GameEngine::serviceWindowsOS() directly from event loop
 *
 * @param event Raw SDL_Event from SDL_PollEvent()
 *
 * GeneralsX @refactor felipebraz 16/02/2026 Fighter19 event model
 */
void SDL3Keyboard::addSDLEvent(SDL_Event *event)
{
	if (!event) {
		return;
	}
	
	// Filter only keyboard-related events
	if (event->type != SDL_EVENT_KEY_DOWN && event->type != SDL_EVENT_KEY_UP) {
		return;  // Not a keyboard event, ignore
	}

	// GeneralsX @bugfix felipebraz 01/04/2026 SDL key-repeat is redundant with engine repeat and can duplicate edits like Backspace.
	if (event->type == SDL_EVENT_KEY_DOWN && event->key.repeat) {
		return;
	}
	
	// Check if buffer is full
	UnsignedInt nextFreeIndex = (m_nextFreeIndex + 1) % MAX_SDL3_KEY_EVENTS;
	if (nextFreeIndex == m_nextGetIndex) {
		// Buffer full, dropping event
		return;
	}
	
	// Copy entire event to buffer
	m_eventBuffer[m_nextFreeIndex] = *event;
	
	// Advance write position (circular buffer)
	m_nextFreeIndex = nextFreeIndex;
}

/**
 * Add SDL3 keyboard event to buffer (LEGACY - kept for compatibility)
 * Called by SDL3GameEngine::handleKeyboardEvent()
 * TODO: Remove after transition to addSDLEvent() is complete
 */
void SDL3Keyboard::addSDL3KeyEvent(const SDL_KeyboardEvent& event)
{
	UnsignedInt nextFreeIndex = (m_nextFreeIndex + 1) % MAX_SDL3_KEY_EVENTS;
	
	// Check if buffer is full
	if (nextFreeIndex == m_nextGetIndex) {
		// Buffer full, dropping key event
		return;
	}
	
	// Create SDL_Event wrapper around SDL_KeyboardEvent
	SDL_Event sdlEvent;
	memset(&sdlEvent, 0, sizeof(sdlEvent));
	sdlEvent.type = event.down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
	sdlEvent.key = event;
	
	// Add to buffer using unified method
	addSDLEvent(&sdlEvent);
}

/**
 * Translate SDL3 scancode to game KeyDefType
 * For Phase 1.5, minimal mapping - full mapping in Phase 2
 */
KeyVal SDL3Keyboard::translateScanCodeToKeyVal(SDL_Scancode scan)
{
	// Quick mapping for essential keys
	// GeneralsX @bugfix felipebraz 01/04/2026 Restore editing/navigation keys required by GUI widgets.
	switch (scan) {
		case SDL_SCANCODE_ESCAPE: return KEY_ESC;      // GeneralsX @bugfix BenderAI 13/02/2026 Fix key constant name
		case SDL_SCANCODE_RETURN: return KEY_ENTER;    // GeneralsX @bugfix BenderAI 13/02/2026 Fix key constant name
		case SDL_SCANCODE_KP_ENTER: return KEY_KPENTER;
		case SDL_SCANCODE_SPACE: return KEY_SPACE;
		case SDL_SCANCODE_TAB: return KEY_TAB;
		case SDL_SCANCODE_BACKSPACE: return KEY_BACKSPACE;
		case SDL_SCANCODE_DELETE: return KEY_DEL;
		case SDL_SCANCODE_HOME: return KEY_HOME;
		case SDL_SCANCODE_END: return KEY_END;
		case SDL_SCANCODE_PAGEUP: return KEY_PGUP;
		case SDL_SCANCODE_PAGEDOWN: return KEY_PGDN;
		case SDL_SCANCODE_LSHIFT: return KEY_LSHIFT;
		case SDL_SCANCODE_RSHIFT: return KEY_RSHIFT;
		case SDL_SCANCODE_LCTRL: return KEY_LCTRL;     // GeneralsX @bugfix BenderAI 13/02/2026 Fix key constant name
		case SDL_SCANCODE_RCTRL: return KEY_RCTRL;     // GeneralsX @bugfix BenderAI 13/02/2026 Fix key constant name
		case SDL_SCANCODE_LALT: return KEY_LALT;       // GeneralsX @bugfix BenderAI 13/02/2026 Fix key constant name
		case SDL_SCANCODE_RALT: return KEY_RALT;       // GeneralsX @bugfix BenderAI 13/02/2026 Fix key constant name
		
		// Arrow keys
		case SDL_SCANCODE_UP: return KEY_UP;
		case SDL_SCANCODE_DOWN: return KEY_DOWN;
		case SDL_SCANCODE_LEFT: return KEY_LEFT;
		case SDL_SCANCODE_RIGHT: return KEY_RIGHT;
		
		// Function keys
		case SDL_SCANCODE_F1: return KEY_F1;
		case SDL_SCANCODE_F2: return KEY_F2;
		case SDL_SCANCODE_F3: return KEY_F3;
		case SDL_SCANCODE_F4: return KEY_F4;
		case SDL_SCANCODE_F5: return KEY_F5;
		case SDL_SCANCODE_F6: return KEY_F6;
		case SDL_SCANCODE_F7: return KEY_F7;
		case SDL_SCANCODE_F8: return KEY_F8;
		case SDL_SCANCODE_F9: return KEY_F9;
		case SDL_SCANCODE_F10: return KEY_F10;
		case SDL_SCANCODE_F11: return KEY_F11;
		case SDL_SCANCODE_F12: return KEY_F12;
		
		// Numbers
		case SDL_SCANCODE_1: return KEY_1;
		case SDL_SCANCODE_2: return KEY_2;
		case SDL_SCANCODE_3: return KEY_3;
		case SDL_SCANCODE_4: return KEY_4;
		case SDL_SCANCODE_5: return KEY_5;
		case SDL_SCANCODE_6: return KEY_6;
		case SDL_SCANCODE_7: return KEY_7;
		case SDL_SCANCODE_8: return KEY_8;
		case SDL_SCANCODE_9: return KEY_9;
		case SDL_SCANCODE_0: return KEY_0;
		
		// Letters (A-Z)
		case SDL_SCANCODE_A: return KEY_A;
		case SDL_SCANCODE_B: return KEY_B;
		case SDL_SCANCODE_C: return KEY_C;
		case SDL_SCANCODE_D: return KEY_D;
		case SDL_SCANCODE_E: return KEY_E;
		case SDL_SCANCODE_F: return KEY_F;
		case SDL_SCANCODE_G: return KEY_G;
		case SDL_SCANCODE_H: return KEY_H;
		case SDL_SCANCODE_I: return KEY_I;
		case SDL_SCANCODE_J: return KEY_J;
		case SDL_SCANCODE_K: return KEY_K;
		case SDL_SCANCODE_L: return KEY_L;
		case SDL_SCANCODE_M: return KEY_M;
		case SDL_SCANCODE_N: return KEY_N;
		case SDL_SCANCODE_O: return KEY_O;
		case SDL_SCANCODE_P: return KEY_P;
		case SDL_SCANCODE_Q: return KEY_Q;
		case SDL_SCANCODE_R: return KEY_R;
		case SDL_SCANCODE_S: return KEY_S;
		case SDL_SCANCODE_T: return KEY_T;
		case SDL_SCANCODE_U: return KEY_U;
		case SDL_SCANCODE_V: return KEY_V;
		case SDL_SCANCODE_W: return KEY_W;
		case SDL_SCANCODE_X: return KEY_X;
		case SDL_SCANCODE_Y: return KEY_Y;
		case SDL_SCANCODE_Z: return KEY_Z;

		// GeneralsX @bugfix Claude 17/08/2026 Finish the table. Every key below is a
		// MappableKeyType (MetaEvent.h) - i.e. the engine allows a command to be bound to it -
		// and every one of them was falling through to KEY_NONE, so the binding could never fire
		// on any SDL3 platform (macOS, Linux, iPadOS). The Windows DirectInput path has always
		// delivered them, which is why this reads as a Mac-only "hotkey doesn't work".
		//
		// Confirmed dead against the SHIPPED CommandMap.ini (Data\English\CommandMap.ini inside
		// W3DEnglishZH.big), not against a guess at the defaults:
		//   KEY_TICK      -> OPTIONS
		//   KEY_BACKSLASH -> CREATE_FORMATION
		//   KEY_MINUS / KEY_EQUAL -> BEGIN/END_PREFER_SELECTION
		//   KEY_KP4/KP6   -> BEGIN/END_CAMERA_ROTATE_LEFT / RIGHT
		//   KEY_KP8/KP2   -> BEGIN/END_CAMERA_ZOOM_IN / OUT
		//   KEY_KP5       -> CAMERA_RESET
		// and against MetaMap::generateMetaMap(), which binds the frame-pacing commands to the
		// keypad +/- :
		//   CTRL+KP_PLUS / CTRL+KP_MINUS             -> INCREASE/DECREASE_MAX_RENDER_FPS
		//   SHIFT+CTRL+KP_PLUS / SHIFT+CTRL+KP_MINUS -> INCREASE/DECREASE_LOGIC_TIME_SCALE
		// The second pair IS the game-speed control, and with no KEY_KPPLUS/KEY_KPMINUS reaching
		// the meta map it could never match - the reported "gamespeed edits dont work".

		// Keypad. KEY_KPENTER is above with the other Enter keys.
		case SDL_SCANCODE_KP_0: return KEY_KP0;
		case SDL_SCANCODE_KP_1: return KEY_KP1;
		case SDL_SCANCODE_KP_2: return KEY_KP2;
		case SDL_SCANCODE_KP_3: return KEY_KP3;
		case SDL_SCANCODE_KP_4: return KEY_KP4;
		case SDL_SCANCODE_KP_5: return KEY_KP5;
		case SDL_SCANCODE_KP_6: return KEY_KP6;
		case SDL_SCANCODE_KP_7: return KEY_KP7;
		case SDL_SCANCODE_KP_8: return KEY_KP8;
		case SDL_SCANCODE_KP_9: return KEY_KP9;
		case SDL_SCANCODE_KP_PERIOD: return KEY_KPDEL;
		case SDL_SCANCODE_KP_MULTIPLY: return KEY_KPSTAR;
		case SDL_SCANCODE_KP_MINUS: return KEY_KPMINUS;
		case SDL_SCANCODE_KP_PLUS: return KEY_KPPLUS;
		case SDL_SCANCODE_KP_DIVIDE: return KEY_KPSLASH;

		// Punctuation / symbol row
		case SDL_SCANCODE_MINUS: return KEY_MINUS;
		case SDL_SCANCODE_EQUALS: return KEY_EQUAL;
		case SDL_SCANCODE_LEFTBRACKET: return KEY_LBRACKET;
		case SDL_SCANCODE_RIGHTBRACKET: return KEY_RBRACKET;
		case SDL_SCANCODE_SEMICOLON: return KEY_SEMICOLON;
		case SDL_SCANCODE_APOSTROPHE: return KEY_APOSTROPHE;
		case SDL_SCANCODE_GRAVE: return KEY_TICK;
		case SDL_SCANCODE_BACKSLASH: return KEY_BACKSLASH;
		case SDL_SCANCODE_COMMA: return KEY_COMMA;
		case SDL_SCANCODE_PERIOD: return KEY_PERIOD;
		case SDL_SCANCODE_SLASH: return KEY_SLASH;

		// The extra key ISO keyboards have next to left shift. Not a MappableKeyType, but
		// Keyboard::initKeyNames() gives it a printable glyph on UK/German/French/Italian
		// layouts, so without this it cannot be typed into any text field.
		case SDL_SCANCODE_NONUSBACKSLASH: return KEY_102;

		case SDL_SCANCODE_INSERT: return KEY_INS;

		// Deliberately NOT mapped, so the reason is on the record rather than looking like
		// another gap in the table:
		//   SDL_SCANCODE_LGUI / RGUI (Command on a Mac, Windows key on a PC) - KeyDefs.h has no
		//     KeyDefType for it at all. Mapping Command onto KEY_LCTRL would make Cmd+1 assign a
		//     control group, which some Mac players will expect, but it is a deliberate change to
		//     what the key bindings MEAN and would silently shadow the OS's own Cmd chords. That
		//     is a product decision, not a bug fix, so it is left out of this change.
		//   SDL_SCANCODE_CAPSLOCK - KEY_CAPS exists, but Keyboard::translateKey() treats it as a
		//     latching toggle driven by press events, and macOS reports caps lock as a
		//     down-on-engage / up-on-disengage pair rather than a press-release. Wiring it up
		//     needs its own test; nothing is bound to it today.
		default:
			return KEY_NONE;
	}
}

#endif // !_WIN32
