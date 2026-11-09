/*
** GeneralsX @feature Claude 23/09/2026 - how much bigger a message box must get to show its body, as
** a pure function.
**
** gogoMessageBox used to grow the box DOWN only, clamped to the screen. That is enough for a few
** sentences and not for the desync report, which is one row per player and a lobby is twelve now
** (MAX_SLOTS). Computed with this engine's FreeType metrics and Build_Sentence_Not_Centered's wrap
** rule, against the three MessageBox.wnd copies a normal install mounts (retail 800x600-authored,
** ControlBar Pro 1920x1080 and its 3840x2160 fix): the rows are wider than the authored body and
** wrap. With ControlBar Pro at 1920x1080 a 20-line report becomes 36 lines, and the last twelve -
** most of the table, including the DIFFERS row - fall below the screen clamp.
**
** Three steps, each taken only when the one before cannot show the whole body:
**
**   1. The body fits the layout as authored: return nothing. Every retail message box, unchanged.
**   2. It does not: widen the box so no line wraps - but ONLY for a body of short lines, and only
**      if that is possible within the screen. A table has short lines and is exactly what
**      wrapping destroys. Prose is a paragraph: unwrapping one turns the box into a banner (a
**      123-character error under ControlBar Pro at 1920x1080 went 540 wide to 1529), so it keeps
**      growing down as it always did. "Short lines" is judged on the whole body: wrapping at the
**      authored width makes it less than three times as tall as unwrapped. A paragraph that
**      overflows a box holding k lines wraps to k+1 or more from one, so every retail and
**      ControlBar Pro layout (k >= 2 at all thirteen resolutions checked) reads as prose; the
**      desync report, measured for two to twelve players on the same layouts, stays under 2.6.
**      Widening never happens partially, so a box is either the retail width or exactly its text.
**   3. Still taller than the screen: step the body's point size down, same face, to no less than
**      three quarters of what the layout asked for. Past that the end of the body is clipped, as
**      before - an unreadably small table is not a better answer than a truncated one.
**
** No new art, no colour, no second control: the box keeps MessageBox.wnd's frame, fonts and
** button, which is what "it must look like the game" rules in and out.
**
** Pure so it can be checked without an engine or a display - scripts/test/message-box-fit-test.cpp.
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
		// ponytail: one ratio for the whole body, so rows followed by a long paragraph (the LAN
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
