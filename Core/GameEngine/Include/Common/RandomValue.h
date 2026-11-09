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

// RandomValue.h
// Random number generation system
// Author: Michael S. Booth, January 1998

#pragma once

#include "Lib/BaseType.h"

extern void InitRandom();
extern void InitRandom( UnsignedInt seed );
extern UnsignedInt GetGameLogicRandomSeed();   ///< Get the seed (used for replays)
extern UnsignedInt GetGameLogicRandomSeedCRC();///< Get the seed (used for CRCs)
extern UnsignedInt GetGameClientRandomSeedCRC();///< CRC of the client stream's state (diagnostics only)

// TheSuperHackers @fix Okladnoj PR #2742 - ported to GeneralsX 04/08/2026.
//
// Lets a call site say WHICH random stream it wants instead of hardcoding one, so that work which
// only exists to place a visual effect can be moved off the LOGIC stream and onto the CLIENT
// stream without changing the shape of the calling code.
//
// Why this matters here, measured on this fork: ParticleSystemManagerDummy::createParticleSystem
// returns nullptr under RETAIL_COMPATIBLE_CRC (ParticleSys.h), and several GameLogic sites draw
// from the LOGIC random INSIDE the `if (particleSystem)` guard. A headless peer therefore skips
// draws that a rendering peer takes, and the shared logic seed separates permanently. A lobby of
// one headless host and three rendering clients desyncs on the first damage effect - which is
// exactly what a four-platform match did on 2026-08-04, at frame 5477, with the three rendering
// peers agreeing with each other and the headless host alone.
//
// Note the split declarations in this fork: the underlying entry points live in
// GameLogic/LogicRandomValue.h and GameClient/ClientRandomValue.h, not here as they do upstream.
// Only the implementations in RandomValue.cpp need them, so nothing has to include either header
// to use this.
struct RandomValueClass
{
	virtual Int GetRandomValueInt( Int lo, Int hi, const char *file, Int line ) const = 0;
	virtual Real GetRandomValueReal( Real lo, Real hi, const char *file, Int line ) const = 0;
};
struct LogicRandomValueClass final : RandomValueClass
{
	virtual Int GetRandomValueInt( Int lo, Int hi, const char *file, Int line ) const override;
	virtual Real GetRandomValueReal( Real lo, Real hi, const char *file, Int line ) const override;
};
struct ClientRandomValueClass final : RandomValueClass
{
	virtual Int GetRandomValueInt( Int lo, Int hi, const char *file, Int line ) const override;
	virtual Real GetRandomValueReal( Real lo, Real hi, const char *file, Int line ) const override;
};

// GeneralsX @feature Claude 06/10/2026 render-split step 2: the simulation's cosmetic stream (RandomValue.cpp), for values
// that only place or time an effect. Never CRC'd; never drawn by the render side.
struct LogicCosmeticRandomValueClass final : RandomValueClass
{
	virtual Int GetRandomValueInt( Int lo, Int hi, const char *file, Int line ) const override;
	virtual Real GetRandomValueReal( Real lo, Real hi, const char *file, Int line ) const override;
};

// use these macros to access the random value functions
#define RandomValueInt(randomValueClass, lo, hi) randomValueClass.GetRandomValueInt( lo, hi, __FILE__, __LINE__ )
#define RandomValueReal(randomValueClass, lo, hi) randomValueClass.GetRandomValueReal( lo, hi, __FILE__, __LINE__ )

//--------------------------------------------------------------------------------------------------------------
