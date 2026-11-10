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

// GeneralsX: WS-G: the S1 DrawRun seat's engine-free logic (rw1-G/PLAN_seat.md D1, D4, D5; CONTRACT_L2 §3.2
// S1-DR, §3.4 under DrawRun). GxScene.cpp drives it from the category task loop; scripts/test/l2-seat-test.cpp drives it by hand.

#pragma once

#include <cstddef>
#include <cstdint>

#ifndef GX_SEAT_MUTANT
#define GX_SEAT_MUTANT 0	// scripts/test/l2-seat-test.sh plants one defect per build
#endif

namespace gxseat {

// D1: a task as the category loop will meet it, in the loop's order from the run's first task.
struct Cand {
	bool overflow;	// Get_Base_Vertex_Offset() == VERTEX_BUFFER_OVERFLOW: the loop skips it (no list entry, no draw)
	bool matches;	// its flush-start list entry is this task (same mesh and polygon renderer at the expected index: the K invariant)
	bool staticOk;	// the flush-start plan's reason for that entry is OK (the static rules; the draw-time ones come after PREPARE)
};
// The index ONE PAST the run's last task in c[0..n) - overflow tasks inside the span are skipped, not members; trailing ones are not
// part of the run. 0 = no run (c[0] itself is not claimable, or is skipped).
size_t runEnd(const Cand *c, size_t n);

// D4: what a task's light set is - the light count, the payloads Set_Light_Environment leaves in slots 0..count-1 (D3DLIGHT8 bytes; a
// slot past the count is disabled and its bytes are not part of the set). The ambient is per task, not per set (DrawRun's own field).
constexpr int kLightBytes = 104;	// sizeof(D3DLIGHT8)
struct LightKey { int count; unsigned char bytes[4 * kLightBytes]; };
// Each task's light-set index into the run's sets: a task whose set equals the PREVIOUS task's shares its index (DrawRun re-applies the
// lights only when the index changes). first[s] = the task that defines set s. Returns the set count; every index < it.
unsigned lightSets(const LightKey *k, size_t n, unsigned *index, unsigned *first);

// D5 (rw1-X2-2 r17 #2/#4 simplification): the C2b digest bracket at GX_DRAWRUN_DIGEST=2 holds the run's OWN draws only, in both toggle
// states, never nested: open before PREPARE; in ON closed right after the CARRY-OUT's Apply (runEnd); in OFF / refused closed before
// right after the run's last own wrapper draw is RECORDED (drawDone; or at the next run's open / the frame's end). There is no successor draw, so no
// bracket closes early. mark(begin, tag) is called back in stream order; only the close's tag names the bracket (DRAWRUN_ABI.md DigestMark).
struct Digest {
	void (*mark)(void *ctx, bool begin, unsigned long long tag) = nullptr;
	void *ctx = nullptr;
	enum : unsigned { CLOSED = 0, OWN };	// open: collecting the run's own draws
	unsigned state = CLOSED, own = 0, frame = 0;
	unsigned long long tag = 0;
	void runBegin(unsigned frameNo);	// before PREPARE: closes any open bracket, then opens
	void runPath(unsigned long long closeTag, unsigned ownDraws);	// after the verdict: the close's tag; the run's own WRAPPER draws
	void runEnd();						// ON: right after the carry-out's Apply
	void draw(unsigned frameNo);		// a wrapper draw is about to be recorded (the GxDrawHook point): frame change / un-ended ON bracket close
	void drawDone(unsigned frameNo);	// a wrapper draw has just been recorded (GxPostDrawHook): the run's last own draw closes the bracket here
	void frameEnd();					// before Present (and the depth shot): an open bracket closes in its own frame (rw1-X2-2)
	void close();
};

} // namespace gxseat
