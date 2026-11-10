// GeneralsX: Building rotation (gx-handoff ai/wh1ter0se-econ/DESIGN.md section 6): the octant of a direction by
// comparisons only (tan 22.5 deg = 0.41421356), so x64 and arm64 pick the same angle - no atan2 in a simulation decision.
#pragma once

inline float gxOctantAngle(float dx, float dy)
{
	const float P = 3.14159265358979f, T = 0.41421356f;
	const float ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
	if (ax == 0.0f && ay == 0.0f)
		return 0.0f;
	if (ay <= ax * T)
		return dx >= 0 ? 0.0f : P;
	if (ax <= ay * T)
		return dy >= 0 ? P / 2 : -P / 2;
	if (dx >= 0)
		return dy >= 0 ? P / 4 : -P / 4;
	return dy >= 0 ? 3 * P / 4 : -3 * P / 4;
}
