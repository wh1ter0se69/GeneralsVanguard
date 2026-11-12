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

/*
** Pure function: how much bigger a message box must get to show its body (scripts/test/message-box-fit-test.cpp).
** 1. Fits as authored: change nothing. 2. Short-line body (a table): widen so nothing wraps, if the screen allows;
** prose keeps growing down. 3. Still too tall: shrink the font to at least 3/4 of the layout's, then clip.
*/

#pragma once

#include "Lib/BaseType.h"

/// Measure the body at pointSize, word-wrapped at wrapWidth pixels (0 = unwrapped: the widest line).
typedef void (*MessageBoxMeasureFunc)( void *ctx, Int pointSize, Int wrapWidth, Int *width, Int *height );

struct MessageBoxFitInput
{
	Int screenW, screenH;
	Int parentW, parentH;			///< the layout's box, already scaled to the screen
	Int messageW, messageH;		///< the layout's body control inside it
	Int margin;								///< TextData::topMargin; the text needs it above and below
	Int pointSize;						///< the size the layout gave the body
};

struct MessageBoxFitResult
{
	Int pointSize;						///< <= the input's; the caller swaps the font only if it moved
	Int growW;								///< add to the box, the body and every full-width child
	Int growH;								///< add to the box and the body; the buttons move down by it
	Bool overflows;						///< taller than the authored control: anchor the text to the top
};

enum
{
	MESSAGE_BOX_SCREEN_MARGIN	= 20,	///< the box stops this far short of the screen, both axes
	MESSAGE_BOX_WRAP_INSET		= 10,	///< drawStaticTextText wraps at the control width minus this
	MESSAGE_BOX_MIN_POINT			= 8
};

inline MessageBoxFitResult MessageBoxFit( const MessageBoxFitInput &in, MessageBoxMeasureFunc measure, void *ctx )
{
	MessageBoxFitResult r = { in.pointSize, 0, 0, FALSE };
	const Int maxW = in.screenW - MESSAGE_BOX_SCREEN_MARGIN;
	const Int maxH = in.screenH - MESSAGE_BOX_SCREEN_MARGIN;

	Int floorPt = ( in.pointSize * 3 + 3 ) / 4;
	if( floorPt < MESSAGE_BOX_MIN_POINT )
		floorPt = MESSAGE_BOX_MIN_POINT;
	if( floorPt > in.pointSize )
		floorPt = in.pointSize;

	for( Int pt = in.pointSize; pt >= floorPt; --pt )
	{
		Int w = 0, h = 0;
		measure( ctx, pt, in.messageW - MESSAGE_BOX_WRAP_INSET, &w, &h );
		if( pt == in.pointSize && h + 2 * in.margin <= in.messageH )
			return r;

		// The wrap test is `cursor + word >= wrapWidth` (Build_Sentence_Not_Centered), so a line
		// exactly as wide as the wrap still breaks: one pixel more than the widest line.
		// Trade-off: one ratio for the whole body, so rows followed by a long paragraph (the LAN
		// SimId-mismatch dialog) read as short lines and still widen to the paragraph, up to the
		// screen. Judging each line needs a per-line measure; add it if that dialog matters.
		const Int wrappedH = h;
		Int growW = 0;
		measure( ctx, pt, 0, &w, &h );
		const Int wantW = w + 1 + MESSAGE_BOX_WRAP_INSET;
		const Bool shortLines = ( wrappedH < 3 * h ) ? TRUE : FALSE;
		if( shortLines && wantW > in.messageW && in.parentW + ( wantW - in.messageW ) <= maxW )
			growW = wantW - in.messageW;

		measure( ctx, pt, in.messageW + growW - MESSAGE_BOX_WRAP_INSET, &w, &h );
		const Int need = h + 2 * in.margin;

		r.pointSize = pt;
		r.growW = growW;
		r.growH = ( need > in.messageH ) ? need - in.messageH : 0;
		r.overflows = ( need > in.messageH ) ? TRUE : FALSE;
		if( in.parentH + r.growH <= maxH )
			return r;
	}

	// Nothing fits at the floor either. Keep the OK button on screen - a box that cannot be
	// dismissed is worse than one missing its last lines - and lose the text from the end.
	if( r.growH > maxH - in.parentH )
		r.growH = maxH - in.parentH;
	if( r.growH < 0 )
		r.growH = 0;
	return r;
}
