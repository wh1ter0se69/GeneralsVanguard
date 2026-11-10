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

// W3DDynamicLight.cpp
// Class to handle dynamic lights.
// Author: John Ahlquist, April 2001
#include <stdlib.h>

#include "W3DDevice/GameClient/W3DDynamicLight.h"
#include "GameLogic/GameLogic.h"	// [class-C gate] the logic frame this light is paced by

W3DDynamicLight::W3DDynamicLight():
LightClass(LightClass::POINT)
{

	m_priorEnable = false;
	m_enabled = true;

}

W3DDynamicLight::~W3DDynamicLight()
{
}

void W3DDynamicLight::On_Frame_Update()
{
	if (!m_enabled) {
		return;
	}
	// GeneralsX: Class-C gate - see m_lastLogicFrameUpdated in the header.
	// Only the two COUNTDOWNS are gated. Every `factor =` line, the enable test and the
	// FarAttenEnd/Ambient/Diffuse writes below stay unconditional, because the renderer re-reads
	// them on every rendered frame and gating them would make the light flicker rather than fade.
	// Falls open when TheGameLogic is absent (WorldBuilder, shell) so behaviour there is unchanged.
	const UnsignedInt gxNow = (TheGameLogic != NULL) ? TheGameLogic->getFrame() : 0;
	const Bool gxAdvanced = (TheGameLogic == NULL) || (gxNow != m_lastLogicFrameUpdated);
	m_lastLogicFrameUpdated = gxNow;

	Real factor = 1.0f;
	if (m_curIncreaseFrameCount>0 && m_increaseFrameCount>0) {
		// increasing
		if (gxAdvanced)
			m_curIncreaseFrameCount--;
		factor = (m_increaseFrameCount-m_curIncreaseFrameCount)/(Real)m_increaseFrameCount;

	}	else if (m_decayFrameCount==0) {
		factor = 1.0;  // never decays,
	}	else {
		if (gxAdvanced)
			m_curDecayFrameCount--;
		if (m_curDecayFrameCount == 0) {
			m_enabled = false;
			return;
		}
		factor = m_curDecayFrameCount/(Real)m_decayFrameCount;
	}
	if (m_decayRange) {
		this->FarAttenEnd = factor*m_targetRange;
		if (FarAttenEnd < FarAttenStart) {
			FarAttenEnd = FarAttenStart;
		}
	}
	if (m_decayColor) {
		this->Ambient = m_targetAmbient*factor;
		this->Diffuse = m_targetDiffuse*factor;
	}
}

void W3DDynamicLight::setFrameFade(UnsignedInt frameIncreaseTime, UnsignedInt decayFrameTime)
{
	m_decayFrameCount = decayFrameTime;
	m_curDecayFrameCount = decayFrameTime;
	m_curIncreaseFrameCount = frameIncreaseTime;
	// GeneralsX: Re-seed the class-C stamp, so a light recycled from the
	// pool steps on its first update instead of losing a frame to a stale stamp.
	m_lastLogicFrameUpdated = 0xffffffff;
	m_increaseFrameCount = frameIncreaseTime;
	m_targetAmbient = Ambient;
	m_targetDiffuse = Diffuse;
	m_targetRange = FarAttenEnd;
}
