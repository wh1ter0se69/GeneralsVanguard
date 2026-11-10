// GeneralsX: Test-only environment knobs: read only in a GX_TEST_INSTRUMENTS build.
//
// The hybrid renderer's DrawRun / L2 seat / interop test paths (synthetic draw-run tests, mutants, truth checks, toggle arms,
// equal/clear/upload tests) were switched on by plain getenv() in the SHIPPING binary: off, they cost an env test, but a stray
// variable in a player's environment could arm a planted defect (GX_SEAT_MUTANT, GX_HYBRID_MUTANT, GX_L2_MUTANT ...). 0.9.63 shipped
// that way and the integrator's follow-up was to gate them. A stock build now sees none of them; a GX_TEST_INSTRUMENTS build
// (cmake/config-build.cmake, never shipped) reads them exactly as before. The product knobs (GX_L2_DRAWRUN, GX_RENDER_HYBRID*,
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
