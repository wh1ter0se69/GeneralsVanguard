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

// The DrawRun seat's engine-free logic (GxL2Seat.h).

#include "W3DDevice/GameClient/GXRender/Scene/GxL2Seat.h"

#include <cstring>

namespace gxseat {

size_t runEnd(const Cand *c, size_t n)
{
	if (n == 0 || c[0].overflow || !c[0].matches || !c[0].staticOk)
		return 0;
	size_t end = 1;
	for (size_t i = 1; i < n; ++i) {
		if (c[i].overflow) {	// the loop skips it: inside the span it is passed over, after the last member it is not the run's
#if GX_SEAT_MUTANT == 1
			end = i + 1;		// M1: a skipped task counted as a member
#endif
			continue;
		}
		if (!c[i].matches || !c[i].staticOk)
			break;
		end = i + 1;
	}
	return end;
}

static bool sameSet(const LightKey &a, const LightKey &b)
{
#if GX_SEAT_MUTANT == 2
	return a.count == b.count;	// M2: the payloads not compared
#elif GX_SEAT_MUTANT == 3
	return a.count == b.count && std::memcmp(a.bytes, b.bytes, sizeof(a.bytes)) == 0;	// M3: the disabled slots' bytes compared too
#else
	return a.count == b.count && std::memcmp(a.bytes, b.bytes, (size_t)a.count * kLightBytes) == 0;
#endif
}

unsigned lightSets(const LightKey *k, size_t n, unsigned *index, unsigned *first)
{
	unsigned sets = 0;
	for (size_t i = 0; i < n; ++i) {
		if (i > 0 && sameSet(k[i], k[i - 1])) {
			index[i] = index[i - 1];
			continue;
		}
		first[sets] = (unsigned)i;
		index[i] = sets++;
	}
	return sets;
}

void Digest::close()
{
	if (state == CLOSED)
		return;
	state = CLOSED;
	if (mark)
		mark(ctx, false, tag);
}

void Digest::frameEnd()
{
#if GX_SEAT_MUTANT == 6
	return;	// M6: the bracket stays open past Present
#endif
	close();
}

void Digest::runBegin(unsigned frameNo)
{
#if GX_SEAT_MUTANT != 5
	close();	// the previous run's bracket (an OFF run waiting for its next trigger) ends before this one opens - never nested (M5)
#endif
	state = OWN;
	own = 0;
	tag = 0;
	frame = frameNo;
	if (mark)
		mark(ctx, true, 0);
}

void Digest::runPath(unsigned long long closeTag, unsigned ownDraws)
{
	tag = closeTag;
	own = ownDraws;
}

// ON: right after the CARRY-OUT's Apply - the run's own draws (DrawRun's) are all inside, nothing else is
void Digest::runEnd()
{
	if (state == OWN && own == 0)
		close();
}

// A wrapper draw is about to be recorded (pre-draw hook). Own draws do not count here: the bracket closes at drawDone.
void Digest::draw(unsigned frameNo)
{
	if (state == CLOSED)
		return;
	if (frameNo != frame) {	// a new frame: the bracket ends before its first draw
		close();
		return;
	}
	if (state != OWN || own == 0) {	// an ON bracket that was not ended (runEnd closes it): ends before this draw
		close();
		return;
	}
#if GX_SEAT_MUTANT == 4
	if (own == 1)
		close();	// M4: the bracket closed BEFORE the run's last own draw is recorded (it falls outside)
#endif
}

// A wrapper draw has just been recorded (post-draw hook, DX8Wrapper::GxPostDrawHook): the run's last own draw closes the bracket HERE -
// direct device draws issued afterwards (water, volumetric shadows, shader-manager quads) are never inside it.
void Digest::drawDone(unsigned frameNo)
{
	if (state != OWN || own == 0)
		return;
	if (frameNo != frame) {
		close();
		return;
	}
	if (--own == 0)
		close();
}

} // namespace gxseat
