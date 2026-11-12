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

// the two decisions of the message list's layout (InGameUI::postDraw) that need a test of their own. Plain ints and
// pointers only, so tests/gxmsglayout/test.cpp builds them without the engine.
#pragma once

// Failed-wrap memory: one entry per (display string, message timestamp), holding the widest room the wrap was measured to fail in.
template <int N> struct GXWrapMemory
{
	const void *ds[N];
	unsigned stamp[N];
	int room[N];
};

// True when this message already failed to wrap in a room as wide as this one (so the wrap width is not toggled again).
template <int N> inline bool gxWrapFailedIn( const GXWrapMemory<N> &m, const void *ds, unsigned stamp, int room )
{
	for( int k = 0; k < N; ++k )
		if( m.ds[ k ] == ds && m.stamp[ k ] == stamp && room <= m.room[ k ] )
			return true;
	return false;
}

// Records a failure. An existing entry for the same message is UPDATED, never duplicated: wider and wider retries of one message
// would otherwise fill every slot and bring back the per-frame wrap toggle.
template <int N> inline void gxWrapNoteFailure( GXWrapMemory<N> &m, const void *ds, unsigned stamp, int room )
{
	int freeSlot = -1;
	for( int k = 0; k < N; ++k )
	{
		if( m.ds[ k ] == ds && m.stamp[ k ] == stamp )
		{
			if( room > m.room[ k ] )
				m.room[ k ] = room;
			return;
		}
		if( m.ds[ k ] == nullptr && freeSlot < 0 )
			freeSlot = k;
	}
	if( freeSlot >= 0 )
	{
		m.ds[ freeSlot ] = ds;
		m.stamp[ freeSlot ] = stamp;
		m.room[ freeSlot ] = room;
	}
}

// A new message took this display string: forget what an earlier message with the same pointer failed. The display-string pool
// hands a just-freed string straight to the next line, and while logic time is frozen the timestamp matches too, so the old entry
// would read as alive and refuse the new line a wrap it has room for.
template <int N> inline void gxWrapForget( GXWrapMemory<N> &m, const void *ds )
{
	for( int k = 0; k < N; ++k )
		if( m.ds[ k ] == ds )
			m.ds[ k ] = nullptr;
}

// The line's top y after clearing the game clock's rectangle (clockBottom <= 0: no clock drawn). Called with the FINAL block size,
// wrapped or not.
inline int gxClearClock( int y, int lineH, int x, int textW, int clockTop, int clockBottom, int clockX0, int clockX1 )
{
	if( clockBottom > 0 && y < clockBottom && y + lineH > clockTop && x < clockX1 && x + textW > clockX0 )
		return clockBottom + 2;
	return y;
}

// The line's top y once clear of the readout row AND the game clock, each { top, bottom, x0, x1 } (bottom <= 0: not drawn). Row,
// clock, row: y only moves down, so the block ends below whichever rectangle moved it last and clear of the other (the clock alone
// could land on the row). Run before the bar decision and again on the FINAL placement with the final size: wrapped,
// unwrapped, or dropped below the bar, which used to skip it.
inline int gxClearRowAndClock( int y, int lineH, int x, int w, const int row[4], const int clock[4] )
{
	y = gxClearClock( y, lineH, x, w, row[ 0 ], row[ 1 ], row[ 2 ], row[ 3 ] );
	y = gxClearClock( y, lineH, x, w, clock[ 0 ], clock[ 1 ], clock[ 2 ], clock[ 3 ] );
	return gxClearClock( y, lineH, x, w, row[ 0 ], row[ 1 ], row[ 2 ], row[ 3 ] );
}
