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

// FILE: CommandXlat.h ///////////////////////////////////////////////////////////
// Author: Steven Johnson, Dec 2001

#pragma once

#include "Common/FrameRateLimit.h"
#include "GameClient/InGameUI.h"

enum GUICommandType CPP_11(: Int);

//-----------------------------------------------------------------------------
class CommandTranslator : public GameMessageTranslator
{
public:

	CommandTranslator();
	virtual ~CommandTranslator() override;

	enum CommandEvaluateType { DO_COMMAND, DO_HINT, EVALUATE_ONLY };


	GameMessage::Type evaluateForceAttack( Drawable *draw, const Coord3D *pos, CommandEvaluateType type );
	GameMessage::Type evaluateContextCommand( Drawable *draw, const Coord3D *pos, CommandEvaluateType type );

private:

	Int m_objective;
	Bool m_teamExists;				///< is there a currently selected "team"?

	// these are for determining if a drag occurred or it was just a sloppy click
	ICoord2D m_rightMouseDownAnchor;		// the location of a possible mouse drag start
	ICoord2D m_rightMouseUpAnchor;			// the location of a possible mouse drag end
	UnsignedInt m_rightMouseDownTimeMs;	// when the mouse down happened
	UnsignedInt m_rightMouseUpTimeMs;		// when the mouse up happened

	GameMessage::Type createMoveToLocationMessage( Drawable *draw, const Coord3D *dest, CommandEvaluateType commandType );
	GameMessage::Type createAttackMessage( Drawable *draw, Drawable *other, CommandEvaluateType commandType );
	GameMessage::Type createEnterMessage( Drawable *enter, CommandEvaluateType commandType );
	GameMessage::Type issueMoveToLocationCommand( const Coord3D *pos, Drawable *drawableInWay, CommandEvaluateType commandType );
	GameMessage::Type issueAttackCommand( Drawable *target, CommandEvaluateType commandType, GUICommandType command = (GUICommandType)0 );
	GameMessage::Type issueSpecialPowerCommand( const CommandButton *command, CommandEvaluateType commandType, Drawable *target, const Coord3D *pos, Object* ignoreSelObj );
	GameMessage::Type issueFireWeaponCommand( const CommandButton *command, CommandEvaluateType commandType, Drawable *target, const Coord3D *pos );
	GameMessage::Type issueCombatDropCommand( const CommandButton *command, CommandEvaluateType commandType, Drawable *target, const Coord3D *pos );

	virtual GameMessageDisposition translateGameMessage(const GameMessage *msg) override;
};


enum FilterTypes CPP_11(: Int)
{
	FT_NULL_FILTER=0,
	// The following are screen filter shaders, that modify the rendered viewport after it is drawn.
	FT_VIEW_BW_FILTER,		//filter to apply a black & white filter to the screen.
	FT_VIEW_MOTION_BLUR_FILTER, //filter to apply motion blur filter to screen.
	FT_VIEW_CROSSFADE,				///<filter to apply a cross blend between previous/current views.
	FT_VIEW_DEFAULT,				///<default filter mode for default filter.
	FT_MAX
};

enum FilterModes CPP_11(: Int)
{
	FM_NULL_MODE = 0,

	// These apply to FT_VIEW_BW_FILTER
	FM_VIEW_BW_BLACK_AND_WHITE, // BW Filter to black & white
	FM_VIEW_BW_RED_AND_WHITE, // BW Filter to red & white
	FM_VIEW_BW_GREEN_AND_WHITE, // BW Filter to green & white

	// These apply to FT_VIEW_CROSSFADE
	FM_VIEW_CROSSFADE_CIRCLE,	// Fades from previous to current view using expanding circle.
	FM_VIEW_CROSSFADE_FB_MASK,	// Fades from previous to current using mask stored in framebuffer alpha.

	// These apply to FT_VIEW_MOTION_BLUR_FILTER
	FM_VIEW_MB_IN_AND_OUT_ALPHA, // Motion blur filter in and out alpha blur
	FM_VIEW_MB_IN_AND_OUT_SATURATE, // Motion blur filter in and out saturated blur
	FM_VIEW_MB_IN_ALPHA, // Motion blur filter in alpha blur
	FM_VIEW_MB_OUT_ALPHA, // Motion blur filter out alpha blur
	FM_VIEW_MB_IN_SATURATE, // Motion blur filter in saturated blur
	FM_VIEW_MB_OUT_SATURATE, // Motion blur filter out saturated blur
	FM_VIEW_MB_END_PAN_ALPHA, // Moton blur on screen pan (for camera tracks object mode)

	FM_VIEW_DEFAULT,	//Default filter that's enabled when all others are off.

	// NOTE: This has to be the last entry in this enum.
	// Add new entries before this one.  jba.
	FM_VIEW_MB_PAN_ALPHA, // Moton blur on screen pan (for camera tracks object mode)

};

class PickAndPlayInfo
{
public:
	PickAndPlayInfo(); //INITIALIZE THE CONSTRUCTOR IN CPP

	Bool						m_air;					//Are we attacking an airborned target?
	Drawable				*m_drawTarget;	//Do we have an override draw target?
	WeaponSlotType	*m_weaponSlot;	//Are we forcing a specific weapon slot? nullptr if unspecified.
	SpecialPowerType m_specialPowerType; //Which special power are use using? SPECIAL_INVALID if unspecified.
};

extern void pickAndPlayUnitVoiceResponse( const DrawableList *list, GameMessage::Type msgType, PickAndPlayInfo *info = nullptr );

// GeneralsX: Expose the frame pacing steps to non-keyboard callers.
//
// These two live in CommandXlat.cpp because the MSG_META_*_MAX_RENDER_FPS / *_LOGIC_TIME_SCALE key
// handlers are their only stock caller, but the pairing between them is subtle enough that it must
// not be reimplemented elsewhere: changeLogicTimeScale re-seeds the scale from the render cap when
// it was disabled, ceils it so the next decrease lands on the right step, toggles the enable flag,
// and refuses outright when TheNetwork is up. A touch-only device has no way to reach any of that,
// so the Debug screen calls the same functions the keys do rather than poking TheFramePacer.
//
// changeMaxRenderFps always succeeds. changeLogicTimeScale returns false and writes nothing while
// TheNetwork is up, so callers that can be reached during a network match must disable their own
// entry point rather than rely on the refusal - a silent no-op reads as a broken control.
extern bool changeMaxRenderFps( FpsValueChange change );
extern bool changeLogicTimeScale( FpsValueChange change );

// GeneralsX: Fast forward (FramePacer::setFastForward): 1x, 2x, 3x, 4x, 5x, 10x, for a live
// match whose only human is this player - a player or an observer, every other slot AI - or a replay. The refusal is
// the reason in words, or nullptr when allowed. Both setters refuse with an on-screen message and return false, and
// the Debug screen greys its buttons on the same refusal.
extern const WideChar *gxFastForwardRefusal();
extern bool setFastForwardSpeed( Int speed );
extern bool changeFastForward( FpsValueChange change );
extern Int (*gxRaceLandingSpeed)();			// GXReplayStrip.inl: > 0 while a replay seek race runs - the fast forward it lands at
extern void (*gxRaceChooseSpeed)(Int);		// ... and where a speed chosen meanwhile goes
