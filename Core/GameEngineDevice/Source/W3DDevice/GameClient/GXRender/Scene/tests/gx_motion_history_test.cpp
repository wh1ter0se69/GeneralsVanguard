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

// gx_motion_history_test: the rules of GxMotionHistory.h
// (§2.3, §9). Plain c++, no engine:
//   c++ -std=c++17 -I<repo>/Core/GameEngineDevice/Include/W3DDevice/GameClient/GXRender/Scene gx_motion_history_test.cpp -o /tmp/t && /tmp/t                 -> PASS, exit 0
//   c++ -std=c++17 -I<same> -DGX_HIST_MUTANT=<1..9> gx_motion_history_test.cpp -o /tmp/t && /tmp/t   -> each plant FAILS, exit 1
#include "GxMotionHistory.h"

#include <cstdio>
#include <cstring>
#include <vector>

using GXRender::GxRigidHistory;
using GXRender::GxSkinHistory;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

static void mat(float m[12], float tx)
{
	std::memset(m, 0, 12 * sizeof(float));
	m[0] = m[5] = m[10] = 1.f;
	m[3] = tx;
}

static int modelA = 0, modelB = 0;

static void rigid()
{
	GxRigidHistory h;
	int a = 0, b = 0;
	float w0[12], w1[12], prev[12];
	mat(w0, 0.f);
	mat(w1, 4.f);

	CHECK(!h.update(&a, &modelA, w0, 10, prev));                 // first sight: not moved
	CHECK(!h.update(&a, &modelA, w0, 11, prev));                 // same matrix next pass: not moved
	std::memset(prev, 0xCD, sizeof(prev));
	CHECK(h.update(&a, &modelA, w1, 12, prev));                  // changed: moved, prev = last pass's matrix
	CHECK(std::memcmp(prev, w0, sizeof(w0)) == 0);
	std::memset(prev, 0xCD, sizeof(prev));
	CHECK(h.update(&a, &modelA, w1, 12, prev));                  // same pass again (another category / pass): the cached answer
	CHECK(std::memcmp(prev, w0, sizeof(w0)) == 0);
	CHECK(!h.update(&a, &modelA, w1, 13, prev));                 // unchanged since 12: not moved
	CHECK(!h.update(&a, &modelA, w0, 15, prev));                 // not seen in 14: a gap is not motion
	CHECK(!h.update(&a, &modelB, w1, 16, prev));                 // the address now holds a mesh of another model: a new history

	CHECK(!h.update(&b, &modelA, w0, 15, prev));
	h.sweep(18);                                        // a last seen in 16, b in 15: stale at 18
	CHECK(h.size() == 0);
	CHECK(!h.update(&a, &modelA, w1, 19, prev));                 // after the sweep a is new again

	// growth keeps every entry findable
	std::vector<int> keys(5000);
	for (size_t i = 0; i < keys.size(); ++i) h.update(&keys[i], &modelA, w0, 20, prev);
	bool all = true;
	for (size_t i = 0; i < keys.size(); ++i) all = h.update(&keys[i], &modelA, w1, 21, prev) && all;
	CHECK(all);
	h.sweep(21);                                        // seen in 21: kept
	CHECK(h.size() == keys.size());
}

static void skin()
{
	GxSkinHistory h;
	int a = 0;
	float p0[6] = { 0, 0, 0, 1, 1, 1 }, p1[6] = { 3, 0, 0, 4, 1, 1 }, out[6];
	float q[9] = { 9, 9, 9, 9, 9, 9, 9, 9, 9 };

	CHECK(!h.exchange(&a, &modelA, p0, 2, 5, out));              // first sight: prev = current
	CHECK(std::memcmp(out, p0, sizeof(p0)) == 0);
	CHECK(h.exchange(&a, &modelA, p1, 2, 6, out));               // next pass, same count: last pass's positions
	CHECK(std::memcmp(out, p0, sizeof(p0)) == 0);
	std::memset(out, 0, sizeof(out));
	CHECK(h.exchange(&a, &modelA, p1, 2, 6, out));               // deformed again in the same pass (W3DView renders the scene twice when a
	CHECK(std::memcmp(out, p0, sizeof(p0)) == 0);       // custom pass is on): still last pass's positions, not camera-only
	float o3[9];
	CHECK(!h.exchange(&a, &modelA, q, 3, 7, o3));                // count changed (LOD switch): current
	CHECK(std::memcmp(o3, q, sizeof(q)) == 0);
	CHECK(!h.exchange(&a, &modelA, p0, 2, 8, out));              // count dropped 3 -> 2 (LOD switch down): current, not the 3-vertex history
	CHECK(std::memcmp(out, p0, sizeof(p0)) == 0);
	CHECK(!h.exchange(&a, &modelA, p0, 2, 10, out));             // gap (not seen in 9): current
	CHECK(std::memcmp(out, p0, sizeof(p0)) == 0);
	CHECK(!h.exchange(&a, &modelB, p1, 2, 11, out));             // another model at the address next pass: current, not a's
	CHECK(std::memcmp(out, p1, sizeof(p1)) == 0);
	h.sweep(13);
	CHECK(h.size() == 0);
}

int main()
{
	rigid();
	skin();
	if (g_fail)
		std::printf("gx_motion_history_test: %d FAILED\n", g_fail);
	else
		std::printf("gx_motion_history_test: PASS\n");
	return g_fail ? 1 : 0;
}
