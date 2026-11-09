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

// GeneralsX @test Claude 02/10/2026 GX_MASSORDER: a scripted, timed mass order for harness matches. See GXMassOrder.cpp.

#pragma once

/// Once per GameClient::update. Returns at once unless GX_MASSORDER is set; then spawns the local player's test army
/// and sends its group orders through TheMessageStream, exactly as the UI would, and prints [GXMASS] timings.
void gxMassOrderUpdate();
