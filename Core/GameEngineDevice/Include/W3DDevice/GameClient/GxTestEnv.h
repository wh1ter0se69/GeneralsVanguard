/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2026 GeneralsX contributors
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

// Test-only environment knobs: read only in a GX_TEST_INSTRUMENTS build.
//
// The hybrid renderer's DrawRun / L2 seat / interop test paths (synthetic draw-run tests, mutants, truth checks, toggle arms,
// equal/clear/upload tests) must not be switched on by plain getenv() in the SHIPPING binary: off, they cost an env test, but a stray
// variable in a player's environment could arm a planted defect (GX_SEAT_MUTANT, GX_HYBRID_MUTANT, GX_L2_MUTANT ...).
// A stock build sees none of them; a GX_TEST_INSTRUMENTS build
// (cmake/config-build.cmake, never shipped) reads them. The product knobs (GX_L2_DRAWRUN, GX_RENDER_HYBRID*,
// GX_DRAWRUN_FAST, census) keep plain getenv().
#pragma once
#include <stdlib.h>

inline const char *GxTestEnv(const char *name)
{
#if defined(GX_TEST_INSTRUMENTS)
	return getenv(name);
#else
	(void)name;
	return nullptr;
#endif
}
