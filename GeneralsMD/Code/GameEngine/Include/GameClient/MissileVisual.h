/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2026 TheSuperHackers
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
// GeneralsX @feature Claude 28/09/2026 The superweapon missiles are DRAWN along a real ballistic flight while the
// simulation flies its own path untouched (the math and the why are in MissileVisualMath.h). Render state only: this
// reads logic state and never writes it, never calls a logic RNG, and never touches Object/Thing/Drawable m_transform
// (logic converts bone positions through the drawable's transform). The result is consumed by Drawable::draw, by
// particle systems attached to the missile, and by the draw walk (GameClient::iterateDrawablesInRegion).
#pragma once

class Drawable;
class Matrix3D;
struct MissileVisual;
enum ObjectShroudStatus CPP_11(: Int);

/// Evaluate the visual flight of `draw` for the current logic frame (cached: repeated calls in one frame are free).
/// Returns the transform to draw at, or null to draw at the object's own transform. A drawable that is not one of the
/// superweapon missiles is classified once and costs a pointer test afterwards.
const Matrix3D *missileVisualUpdate( Drawable *draw, MissileVisual **state, Bool *checked );

/// The transform of the last evaluation, or null.
const Matrix3D *missileVisualTransform( const MissileVisual *state );


void missileVisualFree( MissileVisual **state );

/// GX_MISSILEVIS=1 (drawn) or 0 (computed, not drawn: the before arm): [GXMVIS] trace of every tracked missile.
Bool missileVisualTraceOn();
void missileVisualShockReset();	///< a game ends or a save loads: forget the shockwaves in flight (their particle systems go with the particle manager)
void missileVisualShockTick();	///< every client update: the nuke detonation shockwaves' radii follow the simulation's blast schedule

/// GX_TEST_FIRESW=1: the local player fires its ready nuke / SCUD Storm (test input, recorded like a click). No-op unless set.
void missileVisualTestFire();
