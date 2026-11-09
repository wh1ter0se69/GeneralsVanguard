/*
**	Command & Conquer Generals(tm)
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

////////////////////////////////////////////////////////////////////////////////
//																																						//
//  (c) 2001-2003 Electronic Arts Inc.																				//
//																																						//
////////////////////////////////////////////////////////////////////////////////

// FILE: DiscreteCircle.cpp ////////////////////////////////////////////////////
//-----------------------------------------------------------------------------
//
//                       EA Pacific.
//
//                       Confidential Information
//                Copyright (C) 2002 - All Rights Reserved
//
//-----------------------------------------------------------------------------
//
// Project:   RTS3
//
// File name: DiscreteCircle.cpp
//
// Created:   John McDonald, September 2002
//
// Desc:      ???
//
//-----------------------------------------------------------------------------

#include "PreRTS.h"
#include "Common/DiscreteCircle.h"

//-------------------------------------------------------------------------------------------------
DiscreteCircle::DiscreteCircle(Int xCenter, Int yCenter, Int radius)
{
	m_yPos = yCenter;
	m_yPosDoubled = (yCenter << 1);
	m_edges.reserve(radius << 1);	// largest that it should ever be.

	generateEdgePairs(xCenter, yCenter, radius);
	removeDuplicates();
}

//-------------------------------------------------------------------------------------------------
void DiscreteCircle::drawCircle(ScanlineDrawFunc functionToDrawWith, void *parmToPass)
{
	for (VecHorzLine::const_iterator it = m_edges.begin(); it != m_edges.end(); ++it) {
		(functionToDrawWith)(it->xStart, it->xEnd, it->yPos, parmToPass);
		if (it->yPos != m_yPos) {
			(functionToDrawWith)(it->xStart, it->xEnd, m_yPosDoubled - it->yPos, parmToPass);
		}
	}
}

//-------------------------------------------------------------------------------------------------
void DiscreteCircle::generateEdgePairs(Int xCenter, Int yCenter, Int radius)
{
	// Uses Bresenham to generate points.
	Int x = 0;
	Int y = radius;
	Int d = (1 - radius) << 1;

	while (y >= 0) {
		HorzLine hl;
		hl.xStart = xCenter - x;
		hl.xEnd		= xCenter + x;
		hl.yPos		= yCenter + y;
		m_edges.push_back(hl);

		if (d + y > 0) {
			--y;
			d -= ((y << 1) - 1);
		}

		if (x > d) {
			++x;
			d += ((x << 1) + 1);
		}
	}
}

//-------------------------------------------------------------------------------------------------
// GeneralsX @bugfix Claude 06/10/2026 Each erase shifted the whole rest of the vector, so a large circle cost the square of
// its edge count: a million-cell reveal radius was about 400,000 erases over 1.4 million edges (batch-0958 Codex r3-full).
// Same result in one pass: an edge is kept when it is the last one or the next edge is on another row (the old loop kept
// exactly those, compared against the original next edge), in the same order.
void DiscreteCircle::removeDuplicates()
{
	const size_t count = m_edges.size();
	size_t kept = 0;
	for (size_t i = 0; i < count; ++i) {
		if (i + 1 == count || m_edges[i].yPos != m_edges[i + 1].yPos) {
			m_edges[kept++] = m_edges[i];
		}
	}
	m_edges.resize(kept);
}
