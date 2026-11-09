// GeneralsX @feature tilt-thumb-buttons 06/10/2026 Mouse thumb buttons tilt the camera while held.
// Pure and header-only so scripts/test/thumb-tilt-test.cpp can check the mapping without the engine.
// Client-only: the view is not simulated, nothing here reaches the network or the digest.
#pragma once

// ponytail: the one feel knob. Full 20..60 degree range in ~1.3 s.
static const float GX_THUMB_TILT_RAD_PER_SEC = 0.5236f;	// 30 deg/s
static const float GX_THUMB_TILT_MAX_DT = 0.1f;				// a hitch must not become a jump

// Button 4 (back, X1) flattens the view, button 5 (forward, X2) steepens it - same sign as the wheel
// (scroll up = steeper, see LookAtXlat). Both held, or neither, cancel.
inline int gxThumbTiltDir(bool back, bool forward)
{
	return (forward ? 1 : 0) - (back ? 1 : 0);
}

inline float gxThumbTiltDelta(int dir, float dtSeconds)
{
	const float dt = dtSeconds < 0.0f ? 0.0f : (dtSeconds > GX_THUMB_TILT_MAX_DT ? GX_THUMB_TILT_MAX_DT : dtSeconds);
	return (float)dir * GX_THUMB_TILT_RAD_PER_SEC * dt;
}

// A thumb DOWN that reaches the translator while the window is unfocused must not set the held flag: its UP may
// be lost, and the tilt would run on after refocus. An UP always clears.
inline bool gxThumbPressHeld(bool down, bool lostFocus)
{
	return down && !lostFocus;
}
