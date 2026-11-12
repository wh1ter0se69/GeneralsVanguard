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

// FILE: ExtrasMenu.cpp ////////////////////////////////////////////////////////////////////////////
// Desc:   Extras/QoL settings menu for SagePatch (camera, scroll, terrain, graphics)
///////////////////////////////////////////////////////////////////////////////////////////////////

#include "PreRTS.h"

#include "GameClient/GameClient.h"
#include "Common/GlobalData.h"
#include "Common/OptionPreferences.h"
#include "Common/NameKeyGenerator.h"
#include "GameClient/Shell.h"
#include "GameClient/GUICallbacks.h"
#include "GameClient/GameWindow.h"
#include "GameClient/GameWindowManager.h"
#include "GameClient/Gadget.h"
#include "GameClient/GadgetSlider.h"
#include "GameClient/GadgetPushButton.h"
#include "GameClient/GXHostCamera.h"
#include "GameClient/KeyDefs.h"
#include "GameClient/WindowLayout.h"
#include "GameClient/Display.h"
#include "GameClient/View.h"
#include "GameLogic/GameLogic.h"

// Widget IDs and pointers
static NameKeyType sliderMaxCameraHeightID = NAMEKEY_INVALID;
static NameKeyType sliderMinCameraHeightID = NAMEKEY_INVALID;
static NameKeyType sliderCameraPitchID = NAMEKEY_INVALID;
static NameKeyType sliderScrollSpeedID = NAMEKEY_INVALID;
static NameKeyType sliderDrawDistanceID = NAMEKEY_INVALID;

static GameWindow *sliderMaxCameraHeight = nullptr;
static GameWindow *sliderMinCameraHeight = nullptr;
static GameWindow *sliderCameraPitch = nullptr;
static GameWindow *sliderScrollSpeed = nullptr;
static GameWindow *sliderDrawDistance = nullptr;

static OptionPreferences *pref = nullptr;

// What Accept does with the camera pitch: store it only if the player moved
// the slider, remove it after Defaults, and otherwise leave the file alone - see View::setPrefCameraPitch.
enum PitchEdit { PITCH_UNTOUCHED, PITCH_MOVED, PITCH_DEFAULTS };
static PitchEdit s_pitchEdit = PITCH_UNTOUCHED;

//-------------------------------------------------------------------------------------------------
void ExtrasMenuInit(WindowLayout *layout, void *userData)
{
	sliderMaxCameraHeightID = TheNameKeyGenerator->nameToKey("ExtrasMenu.wnd:SliderMaxCameraHeight");
	sliderMinCameraHeightID = TheNameKeyGenerator->nameToKey("ExtrasMenu.wnd:SliderMinCameraHeight");
	sliderCameraPitchID = TheNameKeyGenerator->nameToKey("ExtrasMenu.wnd:SliderCameraPitch");
	sliderScrollSpeedID = TheNameKeyGenerator->nameToKey("ExtrasMenu.wnd:SliderScrollSpeed");
	sliderDrawDistanceID = TheNameKeyGenerator->nameToKey("ExtrasMenu.wnd:SliderDrawDistance");

	sliderMaxCameraHeight = TheWindowManager->winGetWindowFromId(nullptr, sliderMaxCameraHeightID);
	sliderMinCameraHeight = TheWindowManager->winGetWindowFromId(nullptr, sliderMinCameraHeightID);
	sliderCameraPitch = TheWindowManager->winGetWindowFromId(nullptr, sliderCameraPitchID);
	sliderScrollSpeed = TheWindowManager->winGetWindowFromId(nullptr, sliderScrollSpeedID);
	sliderDrawDistance = TheWindowManager->winGetWindowFromId(nullptr, sliderDrawDistanceID);

	pref = NEW OptionPreferences;

	// Populate sliders from current settings
	if (sliderMaxCameraHeight) {
		Int val = (Int)pref->getMaxCameraHeight();
		GadgetSliderSetPosition(sliderMaxCameraHeight, val);
	}
	if (sliderMinCameraHeight) {
		Int val = (Int)pref->getMinCameraHeight();
		GadgetSliderSetPosition(sliderMinCameraHeight, val);
	}
	// Pitch through View's resolver, rounded (the default 37.5 opens as 38).
	s_pitchEdit = PITCH_UNTOUCHED;
	if (sliderCameraPitch) {
		Int val = (Int)(View::getPrefCameraPitch(*pref) + 0.5f);
		GadgetSliderSetPosition(sliderCameraPitch, val);
	}
	// Round, never truncate: 1.05f * 100.0f is 104.99999, so the
	// shipped 1.05 opened as 104 and every Accept stepped 106 -> 105 -> 104 (scroll speed likewise).
	// Draw distance goes through View's resolver, not the pref getter: the file may still hold the
	// percent this menu used to write, which the getter reads as 2.0.
	if (sliderScrollSpeed) {
		Int val = (Int)(pref->getScrollFactor() * 100.0f + 0.5f);
		GadgetSliderSetPosition(sliderScrollSpeed, val);
	}
	if (sliderDrawDistance) {
		Int val = (Int)(View::getPrefTerrainDrawDistanceScale(*pref) * 100.0f + 0.5f);
		GadgetSliderSetPosition(sliderDrawDistance, val);
	}

	// While host camera limits (GXHostCamera) apply, the sliders would move without the camera budging,
	// so show the values in force and grey them out. Use isOverrideInForce(), not hasHostLimits(): it ignores GX_HOSTCAM.
	if (GXHostCamera::isOverrideInForce()) {
		const GXHostCamera::Limits inForce = GXHostCamera::effectiveLimits();
		if (sliderMaxCameraHeight) {
			GadgetSliderSetPosition(sliderMaxCameraHeight, (Int)inForce.maxHeight);
			sliderMaxCameraHeight->winEnable( FALSE );
		}
		if (sliderMinCameraHeight) {
			GadgetSliderSetPosition(sliderMinCameraHeight, (Int)inForce.minHeight);
			sliderMinCameraHeight->winEnable( FALSE );
		}
	}
	// The same for a pair the Debug menu saved past this screen's range (its mark,
	// View::getPrefDebugCameraHeights): these sliders stop at 1000 / 50, so they could only show it clamped, and an
	// Accept for any other setting would have written the clamp over it. Greyed, left alone.
	else if (View::getPrefDebugCameraHeights(*pref)) {
		if (sliderMaxCameraHeight)
			sliderMaxCameraHeight->winEnable( FALSE );
		if (sliderMinCameraHeight)
			sliderMinCameraHeight->winEnable( FALSE );
	}

	// This screen was created hidden and never shown, so opening
	// Extras rendered nothing at all. Every other menu in this directory unhides its
	// layout and takes focus here — compare KeyboardOptionsMenuInit, which is pushed
	// from the very same Options handler three lines away. Without the focus call the
	// screen also never receives GWM_CHAR, so ExtrasMenuInput's ESC handler was dead
	// too, leaving no way out even by keyboard.
	layout->hide( FALSE );

	GameWindow *parent = TheWindowManager->winGetWindowFromId(
		nullptr, TheNameKeyGenerator->nameToKey( "ExtrasMenu.wnd:ExtrasMenuParent" ) );
	if( parent != nullptr )
		TheWindowManager->winSetFocus( parent );
}

//-------------------------------------------------------------------------------------------------
void ExtrasMenuUpdate(WindowLayout *layout, void *userData)
{
}

//-------------------------------------------------------------------------------------------------
void ExtrasMenuShutdown(WindowLayout *layout, void *userData)
{
	sliderMaxCameraHeight = nullptr;
	sliderMinCameraHeight = nullptr;
	sliderCameraPitch = nullptr;
	sliderScrollSpeed = nullptr;
	sliderDrawDistance = nullptr;

	if (pref) {
		delete pref;
		pref = nullptr;
	}

	// The Back button did nothing because this never told the shell
	// the shutdown had finished. Shell::pop() hides the screen and then waits for
	// shutdownComplete() before unlinking it and revealing what is underneath; 29 of
	// the 30 menus in this directory make that call and this one did not, so the pop
	// stalled permanently and the player was stuck on the screen. Both the Back and
	// Accept paths route through here, so this fixes both.
	layout->hide( TRUE );
	TheShell->shutdownComplete( layout );
}

//-------------------------------------------------------------------------------------------------
static void saveExtras()
{
	if (!pref)
		return;

	Int val;

	// Not cosmetic: ExtrasMenuInit parks the two camera-height sliders on the host's values while an override is in force, so Accept would write the host's limits into the player's own Options.ini for every future game. Disabling a gadget does not stop GadgetSliderGetPosition from reporting where it sits.
	// The same applies to a pair the Debug menu owns (see ExtrasMenuInit).
	const Bool hostOwnsCameraHeights = GXHostCamera::isOverrideInForce() || View::getPrefDebugCameraHeights(*pref);

	val = GadgetSliderGetPosition(sliderMaxCameraHeight);
	if (val > 0 && !hostOwnsCameraHeights) {
		TheWritableGlobalData->m_maxCameraHeight = (Real)val;
		AsciiString prefString;
		prefString.format("%d", val);
		(*pref)["MaxCameraHeight"] = prefString;
	}

	val = GadgetSliderGetPosition(sliderMinCameraHeight);
	if (val > 0 && !hostOwnsCameraHeights) {
		TheWritableGlobalData->m_minCameraHeight = (Real)val;
		AsciiString prefString;
		prefString.format("%d", val);
		(*pref)["MinCameraHeight"] = prefString;
	}

	// Through View's one writer, and only for a slider the player moved or
	// reset: an untouched one used to save its truncated position ("37") on every Accept. The live view takes
	// the resolved pitch below; TheGlobalData's pitch stays the data default (see View::init).
	val = GadgetSliderGetPosition(sliderCameraPitch);
	const Bool pitchEdited = s_pitchEdit != PITCH_UNTOUCHED;
	if (s_pitchEdit == PITCH_DEFAULTS)
		View::clearPrefCameraPitch(*pref);
	else if (s_pitchEdit == PITCH_MOVED && val > 0)
		View::setPrefCameraPitch(*pref, val);

	val = GadgetSliderGetPosition(sliderScrollSpeed);
	if (val > 0) {
		TheWritableGlobalData->m_keyboardScrollFactor = val / 100.0f;
		AsciiString prefString;
		prefString.format("%d", val);
		(*pref)["ScrollFactor"] = prefString;
	}

	val = GadgetSliderGetPosition(sliderDrawDistance);
	if (val > 0) {
		TheWritableGlobalData->m_terrainDrawDistanceScale = val / 100.0f;
		// Write the real value, as the Debug menu does: "%d" wrote the percent, which every reader clamps to the 2.0 maximum.
		AsciiString prefString;
		prefString.format("%.2f", val / 100.0f);
		(*pref)["TerrainDrawDistanceScale"] = prefString;
	}

	// Push the camera settings into the live view. Same defect
	// as the Debug menu sliders: the view caches the height limits and the default pitch at
	// construction and never re-reads TheGlobalData, so writing the global here did nothing until
	// the next game start re-created the view. See DebugMenu.cpp for the full note.
	// Keep the height, pinned into the new limits, and put a moved pitch live -
	// setZoom(getZoom()) lifted the camera ~44% past the limit on every Accept at 16:9. See DebugMenu.cpp.
	if (TheTacticalView != nullptr) {
		TheTacticalView->setCameraHeightAboveGroundLimitsToDefault();
		TheTacticalView->setDefaultPitch(DEG_TO_RADF(View::getPrefCameraPitch(*pref)));

		if (TheGameLogic != nullptr && TheGameLogic->isInGame() && !TheGameLogic->isInShellGame()) {
			TheTacticalView->setHeightAboveGround(TheTacticalView->getHeightAboveGround());
			const Real maxAbove = TheTacticalView->getZoomReferenceHeight();	// re-base the zoom: see DebugMenu.cpp
			if (maxAbove > 0.0f)
				TheTacticalView->setZoom(TheTacticalView->getHeightAboveGround() / maxAbove);
			if (pitchEdited)
				TheTacticalView->setPitch(TheTacticalView->getDefaultPitch());
		}
	}
}

//-------------------------------------------------------------------------------------------------
static void setDefaults()
{
	// Defaults must not move a greyed slider. Moving it would
	// show a number that is not in force, on a control the player cannot touch, and Accept would
	// then correctly ignore it - which is the same silent no-op in a different costume.
	const Bool hostOwnsCameraHeights = GXHostCamera::isOverrideInForce() || (pref != nullptr && View::getPrefDebugCameraHeights(*pref));

	if (sliderMaxCameraHeight && !hostOwnsCameraHeights)
		GadgetSliderSetPosition(sliderMaxCameraHeight, 500);
	if (sliderMinCameraHeight && !hostOwnsCameraHeights)
		GadgetSliderSetPosition(sliderMinCameraHeight, 80);
	// Show the data default, rounded, and have Accept remove the choice. The
	// old 37 was stored as a choice below the default, in the larger LOW_ANGLE terrain window.
	if (sliderCameraPitch) {
		GadgetSliderSetPosition(sliderCameraPitch, (Int)(TheGlobalData->m_cameraPitch + 0.5f));
		s_pitchEdit = PITCH_DEFAULTS;
	}
	if (sliderScrollSpeed)
		GadgetSliderSetPosition(sliderScrollSpeed, 100);
	if (sliderDrawDistance)
		GadgetSliderSetPosition(sliderDrawDistance, 105);
}

//-------------------------------------------------------------------------------------------------
WindowMsgHandledType ExtrasMenuSystem(GameWindow *window, UnsignedInt msg,
																				WindowMsgData mData1, WindowMsgData mData2)
{
	static NameKeyType buttonBack = NAMEKEY_INVALID;
	static NameKeyType buttonDefaults = NAMEKEY_INVALID;
	static NameKeyType buttonAccept = NAMEKEY_INVALID;

	switch (msg) {

		case GWM_CREATE:
		{
			buttonBack = TheNameKeyGenerator->nameToKey("ExtrasMenu.wnd:ButtonBack");
			buttonDefaults = TheNameKeyGenerator->nameToKey("ExtrasMenu.wnd:ButtonDefaults");
			buttonAccept = TheNameKeyGenerator->nameToKey("ExtrasMenu.wnd:ButtonAccept");
			break;
		}

		case GWM_DESTROY:
			break;

		case GWM_INPUT_FOCUS:
		{
			if (mData1 == TRUE)
				*(Bool *)mData2 = TRUE;
			return MSG_HANDLED;
		}

		case GBM_SELECTED:
		{
			GameWindow *control = (GameWindow *)mData1;
			Int controlID = control->winGetWindowId();

			if (controlID == buttonBack) {
				TheShell->pop();
			}
			else if (controlID == buttonAccept) {
				saveExtras();
				if (pref) {
					pref->write();
				}
				TheShell->pop();
			}
			else if (controlID == buttonDefaults) {
				setDefaults();
			}
			break;
		}

		// A pitch the player moved, as opposed to one Accept merely finds
		// there. The slider gadget reports every move it makes (drag, click, arrow key) to its owner, this
		// window; a programmatic GadgetSliderSetPosition moves it without a report.
		case GSM_SLIDER_TRACK:
		case GSM_SLIDER_DONE:
		{
			if (sliderCameraPitch != nullptr && (GameWindow *)mData1 == sliderCameraPitch)
				s_pitchEdit = PITCH_MOVED;
			break;
		}

		default:
			break;
	}

	return MSG_IGNORED;
}

//-------------------------------------------------------------------------------------------------
WindowMsgHandledType ExtrasMenuInput(GameWindow *window, UnsignedInt msg,
																				WindowMsgData mData1, WindowMsgData mData2)
{
	switch (msg) {

		case GWM_CHAR:
		{
			UnsignedByte key = mData1;
			UnsignedByte state = mData2;

			switch (key) {

				case KEY_ESC:
				{
					if (BitIsSet(state, KEY_STATE_UP)) {
						TheShell->pop();
					}
					return MSG_HANDLED;
				}

			}
		}

	}

	return MSG_IGNORED;
}
