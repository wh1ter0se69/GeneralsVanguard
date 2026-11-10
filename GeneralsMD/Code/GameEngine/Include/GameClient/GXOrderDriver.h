/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 TheSuperHackers
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

// GeneralsX: GX_ORDERS: a scripted human for multiplayer test matches. See GXOrderDriver.cpp.

#pragma once

/// Once per GameClient::update. Returns at once unless GX_ORDERS is set; then issues this peer's local player's
/// orders for the logic frame through TheMessageStream, exactly as the UI would.
void gxOrderDriverUpdate();

// GeneralsX: The driver's one-peer-safe reads, shared with the ai-gym bot input (GXGym.cpp) so both
// order sources answer "could a human click this?" with the same code. See GXOrderDriver.cpp for why each is safe.
class Object;
class Player;
class ThingTemplate;
struct Coord3D;
Bool gxOrderIsVisibleTo(const Object *o, const Player *me);		///< clear cell for me, not an undetected stealther
Bool gxOrderIsEnemyTarget(const Object *o, const Player *me);	///< a live enemy of mine a click could target
Bool gxOrderAnyObjectNear(const Coord3D &at, const ThingTemplate *what);	///< TRUE: the legality check would touch the shroud cache
