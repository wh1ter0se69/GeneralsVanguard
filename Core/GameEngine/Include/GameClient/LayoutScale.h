/*
** GeneralsX: the map from a .wnd's authored coordinates to screen
** pixels, as a pure function.
**
** It lived inline in parseScreenRect, which meant the only way to check it was to run the game and
** read a rect back off a live window. It is arithmetic over six integers and needs neither a
** display nor an engine.
**
** STRETCH is what shipped: each axis is scaled by its own ratio, so a 4:3-authored layout on a 16:9
** display comes out 33% wider than it was drawn - MainMenu.wnd's 208x36 button (aspect 5.78) is
** drawn 499x65 (aspect 7.68), and 7.68/5.78 is exactly 16:9 over 4:3. UNIFORM uses one factor for
** both axes, which keeps every widget's shape, and anchors each GROUP of controls against the edge
** the art puts it against - see the anchor note and the GROUPS block below. It centred the whole
** creation-resolution box originally; that is what pushed right-hand menus 240px inboard.
*/

#pragma once

#include "Lib/BaseType.h"

#include <stdlib.h>
#include <string.h>

enum LayoutScaleMode
{
	LAYOUT_SCALE_STRETCH = 0,	///< per-axis; bit-for-bit what the game shipped with
	LAYOUT_SCALE_UNIFORM = 1	///< one factor both axes, creation box centred on the display
};

struct LayoutRect
{
	Int loX, loY, hiX, hiY;
};

struct LayoutScaleContext
{
	Int							createW, createH;		///< the layout's CREATIONRESOLUTION
	Int							displayW, displayH;	///< the live display
	LayoutScaleMode	mode;
	Real						uiScale;						///< multiplies the uniform factor; ignored when STRETCH
};

/**
** The active mode and dial, read from the environment ONCE per process.
**
** Here rather than in the parser because ControlBarScheme places ~10 HUD controls itself and needs
** the same answer; two copies of "which mode are we in" is how one half of the screen ends up
** scaled differently from the other, which is the exact fault this file exists to remove.
**
** Once, not per call: a layout re-parsed at a different setting mid-session would leave two screens
** disagreeing with each other.
*/
inline LayoutScaleMode LayoutScaleActiveMode( void )
{
	static Int s_mode = -1;
	if( s_mode < 0 )
	{
		const char *env = getenv( "GX_UI_LAYOUT" );
		s_mode = ( env != NULL && strcmp( env, "uniform" ) == 0 )
						 ? (Int)LAYOUT_SCALE_UNIFORM : (Int)LAYOUT_SCALE_STRETCH;
	}
	return (LayoutScaleMode)s_mode;
}

/// Raw GX_UI_SCALE; unset, unparseable or <= 0 means 1.0. The 0.05 floor is LayoutScaleClampDial's.
inline Real LayoutScaleActiveDial( void )
{
	static Real s_dial = -1.0f;
	if( s_dial < 0.0f )
	{
		const char *env = getenv( "GX_UI_SCALE" );
		s_dial = ( env != NULL ) ? (Real)atof( env ) : 1.0f;
		if( s_dial <= 0.0f )
			s_dial = 1.0f;
	}
	return s_dial;
}

/**
** The dial, clamped. ONE definition, because there are two consumers.
**
** Found by a cross-model review: the rect path clamped and the parser's font path multiplied by the
** raw value, so GX_UI_SCALE=0.01 gave boxes at 0.05 and text at 0.01 - text floored to 1pt inside
** boxes twenty times too big for it. Two copies of a clamp is one copy too many.
*/
inline Real LayoutScaleClampDial( Real dial )
{
	return ( dial < 0.05f ) ? 0.05f : dial;
}

/**
** Does this window cover the whole authored canvas?
**
** Such a window is a layout root or a full-screen backdrop, and it must keep filling the display in
** every mode - a uniformly scaled backdrop would leave the desktop showing behind the menu.
**
** ALL FOUR EDGES are tested, not the width and height. Size alone says a rect is canvas-sized, not
** that it is canvas-ALIGNED: {-100,0,700,600} is 800x600 on an 800x600 canvas and reaches neither
** the left edge nor the right one. Raised by a cross-model review, and confirmed by arithmetic
** here rather than by argument - the previous one-sided origin test read -100 * 100 <= 800 as true.
**
** The tolerance is 1%, not exact: LanGameOptionsMenu.wnd's root is authored 0,0..799,599 against a
** CREATIONRESOLUTION of 800x600, while MainMenu.wnd's is a clean 0,0..800,600. An exact test passes
** the main menu and fails the LAN lobby, which is the kind of difference that ships as "the
** background is broken on one screen". Checked against 576 windows across 11 layouts: this selects
** the layout roots and nothing else.
*/
inline Bool LayoutScaleIsFullBleedIn( const LayoutRect &a, Int createW, Int createH )
{
	const Bool leftReaches	 = ( a.loX >= 0 && a.loX * 100 <= createW ) ? TRUE : FALSE;
	const Bool topReaches		 = ( a.loY >= 0 && a.loY * 100 <= createH ) ? TRUE : FALSE;
	const Bool rightReaches	 = ( a.hiX * 100 >= createW * 99 ) ? TRUE : FALSE;
	const Bool bottomReaches = ( a.hiY * 100 >= createH * 99 ) ? TRUE : FALSE;

	return ( leftReaches && topReaches && rightReaches && bottomReaches ) ? TRUE : FALSE;
}

/// The same test against a context. The grouping pass below has a per-rect CREATIONRESOLUTION and
/// no context, which is the only reason the two-argument form above exists.
inline Bool LayoutScaleIsFullBleed( const LayoutScaleContext &ctx, const LayoutRect &a )
{
	return LayoutScaleIsFullBleedIn( a, ctx.createW, ctx.createH );
}

/* ================================================================================================
** GROUPS: the set of controls that must share one anchor.
**
** WHY THIS EXISTS. The anchor rule below used to be computed from each rect ALONE, and the comment
** on it claimed "a panel must not tear along this rule ... every control in a column shares [the
** edges], so a column cannot split". That reasoning is about COLUMNS and rows falsify it.
** ExtrasMenu.wnd authors five label/slider pairs like this:
**
**     SCREENRECT = UPPERLEFT: 100 120, BOTTOMRIGHT: 350 145;     <- label,  loX 100 -> anchors LEFT
**     SCREENRECT = UPPERLEFT: 370 120, BOTTOMRIGHT: 650 145;     <- slider, hiX 650 -> anchors RIGHT
**
** At 1920x1080 from an 800x600 canvas the authored 20-unit gap opens to 366px. Centring BOTH does
** not fix it: the anchor moves by the per-axis scale (2.400) while the size moves by the uniform
** scale (1.800), so the label's right edge lands at 765 and the slider's left at 972 - still a
** 207px tear. Two rects keep their authored gap if and only if they share ONE anchor point, at
** which point the gap is (b.loX - a.hiX) * s by construction, whatever the anchor is.
**
** WHAT A GROUP IS. Two rects belong together when they share an authored edge PAIR:
**
**   * identical (loY,hiY) - they are the same ROW  (the label/slider defect)
**   * identical (loX,hiX) - they are the same COLUMN
**
** and a group is the transitive closure, i.e. a connected component. Equality is an equivalence
** relation, so the partition is well defined and does not depend on the order rects arrive in - a
** tolerance band would not be transitive (A~B and B~C without A~C) and would chain across a screen.
**
** COLUMNS ARE NOT OPTIONAL. Measured over the 80 shipped layouts in WindowZH.big: rows alone fix
** all 186 torn rows, but on ScoreScreen.wnd it anchors the column-header row (bbox 184..756 -> R)
** differently from the eight data rows (bbox 65..756 -> C) and detaches every header from its
** column by 207px - the same defect, rotated. Closing over columns as well merges those 79 windows
** into one group and they move together. It also keeps MainMenu.wnd's right-hand button stack
** together as one 24-member group anchored at 756, so the right-edge behaviour this mode exists for
** is preserved rather than centred away.
**
** WHERE THE GROUP COMES FROM. The parser maps one window at a time as the file is read, and parent
** and sibling links are applied afterwards, so at map time there is nothing to group ON. The caller
** therefore does a cheap FIRST PASS over the layout text - SCREENRECT lines only, no window
** creation - and hands the rects here; the real parse is unchanged and simply looks each rect up.
** A caller that supplies no table (ControlBarScheme places its ~10 HUD controls with no layout file)
** gets exactly today's per-rect behaviour.
** ============================================================================================= */

enum { LAYOUT_SCALE_GROUP_CAP = 512 };			///< 4.5x the largest shipped layout (ScoreScreen, 113)

/**
** Every authored rect in one layout, plus the union-find that partitions them into groups.
**
** Owned by the caller, not by this header: the table is ~22KB and the ControlBar path has no
** layout file and must not pay for it.
*/
struct LayoutScaleGroupTable
{
	Int				 count;
	Bool			 overflowed;										///< past the cap: the WHOLE layout falls back to per-rect
	LayoutRect authored[ LAYOUT_SCALE_GROUP_CAP ];
	Int				 createW [ LAYOUT_SCALE_GROUP_CAP ];
	Int				 createH [ LAYOUT_SCALE_GROUP_CAP ];
	Int				 link		 [ LAYOUT_SCALE_GROUP_CAP ];	///< union-find parent
	LayoutRect box		 [ LAYOUT_SCALE_GROUP_CAP ];	///< the component's bounding box; valid at its root
};

inline void LayoutScaleGroupsReset( LayoutScaleGroupTable *t )
{
	t->count			= 0;
	t->overflowed = FALSE;
}

inline void LayoutScaleGroupsAdd( LayoutScaleGroupTable *t, const LayoutRect &r, Int cw, Int ch )
{
	if( cw <= 0 || ch <= 0 )
		return;
	if( t->count >= LAYOUT_SCALE_GROUP_CAP )
	{
		t->overflowed = TRUE;
		return;
	}
	const Int i			= t->count++;
	t->authored[i]	= r;
	t->createW[i]		= cw;
	t->createH[i]		= ch;
	t->link[i]			= i;
}

inline Int LayoutScaleGroupRoot( LayoutScaleGroupTable *t, Int i )
{
	while( t->link[i] != i )
	{
		t->link[i] = t->link[ t->link[i] ];		// path halving
		i = t->link[i];
	}
	return i;
}

inline void LayoutScaleGroupJoin( LayoutScaleGroupTable *t, Int a, Int b )
{
	a = LayoutScaleGroupRoot( t, a );
	b = LayoutScaleGroupRoot( t, b );
	if( a != b )
		t->link[b] = a;
}

/**
** Close the row/column relation and compute each component's bounding box.
**
** O(n^2) over at most 512 rects, once per layout LOAD - about 130k integer compares on the largest
** shipped file, against a parse that allocates a window object per entry. A hash would be faster
** and would put a container in a header two engines include; the arithmetic does not justify it.
**
** Full-bleed rects take no part. They never reach the anchor code (LayoutScaleMapRect snaps them to
** the display before it gets there), and letting a canvas-sized root into the closure would merge
** every rect that happens to share its 0..800 span into one screen-wide group.
*/
inline void LayoutScaleGroupsBuild( LayoutScaleGroupTable *t )
{
	Int i, j;

	for( i = 0; i < t->count; ++i )
	{
		if( LayoutScaleIsFullBleedIn( t->authored[i], t->createW[i], t->createH[i] ) )
			continue;

		for( j = i + 1; j < t->count; ++j )
		{
			if( t->createW[j] != t->createW[i] || t->createH[j] != t->createH[i] )
				continue;																	// different canvases cannot share an edge
			if( LayoutScaleIsFullBleedIn( t->authored[j], t->createW[j], t->createH[j] ) )
				continue;

			const Bool sameRow = ( t->authored[i].loY == t->authored[j].loY &&
														 t->authored[i].hiY == t->authored[j].hiY ) ? TRUE : FALSE;
			const Bool sameCol = ( t->authored[i].loX == t->authored[j].loX &&
														 t->authored[i].hiX == t->authored[j].hiX ) ? TRUE : FALSE;

			if( sameRow || sameCol )
				LayoutScaleGroupJoin( t, i, j );
		}
	}

	for( i = 0; i < t->count; ++i )
		t->box[i] = t->authored[i];

	for( i = 0; i < t->count; ++i )
	{
		const Int r = LayoutScaleGroupRoot( t, i );
		if( r == i )
			continue;
		if( t->authored[i].loX < t->box[r].loX ) t->box[r].loX = t->authored[i].loX;
		if( t->authored[i].loY < t->box[r].loY ) t->box[r].loY = t->authored[i].loY;
		if( t->authored[i].hiX > t->box[r].hiX ) t->box[r].hiX = t->authored[i].hiX;
		if( t->authored[i].hiY > t->box[r].hiY ) t->box[r].hiY = t->authored[i].hiY;
	}
}

/**
** The group box for an authored rect, or FALSE when it has none.
**
** Matched on the rect's own four numbers rather than on a parse index, so the caller does not have
** to keep the two passes in step - a mis-step there would silently pair a control with a stranger's
** group, which is exactly the class of bug this file is fixing. Duplicate authored rects are common
** (GameSpyGameOptionsMenu.wnd has 79..223 twice on one row) and are harmless: identical rects are
** joined by BOTH the row and the column test, so every copy is always in the same component.
*/
inline Bool LayoutScaleGroupFind( LayoutScaleGroupTable *t, const LayoutScaleContext &ctx,
																	const LayoutRect &a, LayoutRect *groupOut )
{
	if( t->overflowed )
		return FALSE;

	for( Int i = 0; i < t->count; ++i )
	{
		if( t->createW[i] != ctx.createW || t->createH[i] != ctx.createH )
			continue;
		if( t->authored[i].loX == a.loX && t->authored[i].loY == a.loY &&
				t->authored[i].hiX == a.hiX && t->authored[i].hiY == a.hiY )
		{
			*groupOut = t->box[ LayoutScaleGroupRoot( t, i ) ];
			return TRUE;
		}
	}
	return FALSE;
}

/**
** Pass one: pull every SCREENRECT out of a .wnd's raw text.
**
** Deliberately the weakest possible reader - it finds the token, then takes the next six signed
** integers up to the terminating ';'. It does not know what a WINDOW is, does not nest, and does
** not need to: a group is a set of authored rects and nothing else. That is also why it cannot
** drift out of step with the real parser in any way that matters. A malformed entry it cannot read
** is skipped, which costs that one rect its group and leaves it on today's behaviour.
*/
inline void LayoutScaleGroupsScanLayout( LayoutScaleGroupTable *t, const char *text, Int len )
{
	static const char kTok[] = "SCREENRECT";
	const Int kTokLen = (Int)( sizeof( kTok ) - 1 );

	LayoutScaleGroupsReset( t );
	if( text == NULL || len <= 0 )
		return;

	Int p = 0;
	while( p + kTokLen <= len )
	{
		if( text[p] != 'S' || strncmp( text + p, kTok, (size_t)kTokLen ) != 0 )
		{
			++p;
			continue;
		}
		p += kTokLen;

		Int v[6];
		Int got = 0;
		while( got < 6 && p < len && text[p] != ';' )
		{
			const Bool neg = ( text[p] == '-' && p + 1 < len &&
												 text[p+1] >= '0' && text[p+1] <= '9' ) ? TRUE : FALSE;
			if( neg )
				++p;
			if( p < len && text[p] >= '0' && text[p] <= '9' )
			{
				Int n = 0;
				while( p < len && text[p] >= '0' && text[p] <= '9' )
					n = n * 10 + (Int)( text[p++] - '0' );
				v[got++] = neg ? -n : n;
			}
			else
			{
				++p;
			}
		}

		if( got == 6 )
		{
			LayoutRect r;
			r.loX = v[0]; r.loY = v[1]; r.hiX = v[2]; r.hiY = v[3];
			LayoutScaleGroupsAdd( t, r, v[4], v[5] );
		}
	}

	LayoutScaleGroupsBuild( t );
}

/**
** Map an authored rect into absolute screen pixels.
**
** The (Int) casts truncate toward zero and are deliberately the same casts the inline code used.
** Width is derived by subtracting two already-truncated edges rather than by scaling the width, so
** the rounding lands exactly where it did before; doing it the other way is a one-pixel drift on
** every control in the game.
**
** `group`, when non-NULL, is the bounding box of every control that must move as one unit with this
** one; the anchor is chosen from IT and applied to this rect. NULL means "this rect is its own
** group", which is what every caller did before groups existed.
*/
inline void LayoutScaleMapRectGrouped( const LayoutScaleContext &ctx, const LayoutRect &authored,
																			 const LayoutRect *group, LayoutRect *out )
{
	const Real xScale = (Real)ctx.displayW / (Real)ctx.createW;
	const Real yScale = (Real)ctx.displayH / (Real)ctx.createH;

	// What shipped. First and unconditional, so the default cannot be reached by any later branch.
	if( ctx.mode == LAYOUT_SCALE_STRETCH )
	{
		out->loX = (Int)( (Real)authored.loX * xScale );
		out->loY = (Int)( (Real)authored.loY * yScale );
		out->hiX = (Int)( (Real)authored.hiX * xScale );
		out->hiY = (Int)( (Real)authored.hiY * yScale );
		return;
	}

	// A window that covers the canvas must COVER the display, not merely be scaled up: scaling the
	// 799x599 root's short edges would leave 3px down the right and 2px along the bottom bare, by
	// the very branch whose purpose is coverage.
	if( LayoutScaleIsFullBleed( ctx, authored ) )
	{
		out->loX = 0;
		out->loY = 0;
		out->hiX = ctx.displayW;
		out->hiY = ctx.displayH;
		return;
	}

	// One factor, so a square stays square. min() rather than max(): the larger ratio would push
	// the other axis off the display, and on 16:9 from 4:3 that is the whole bottom of the screen.
	Real s = ( ( xScale < yScale ) ? xScale : yScale );

	// Clamped, not trusted - this arrives from an env var. A zero maps every control to a
	// zero-width rect, i.e. a menu that is present, focusable and completely invisible, which is a
	// far worse failure than a menu at the wrong size.
	s *= LayoutScaleClampDial( ctx.uiScale );

	// WHERE THE ANCHOR POINT GOES.
	//
	// Centring the whole canvas was the first answer and it has one visible cost: a 4:3 canvas on a
	// 16:9 display has 480px of horizontal slack at dial 1.0, and splitting it evenly pushes a menu
	// authored against the right edge 240px inboard. The dial makes that worse rather than better,
	// because a smaller canvas leaves more slack to split. Measured on a 1920x1080 client, the main
	// menu's right edge sat 125px from the screen edge under stretch and 334 / 428 / 490 / 553px
	// under uniform at dial 1.00 / 0.85 / 0.75 / 0.65.
	//
	// Giving the slack to the display edge instead was the second answer and it OVERSHOOTS, for a
	// reason worth writing down: the layout ROOT is full-bleed, so the backdrop and the border art
	// drawn into it stretch by the per-axis xScale whatever the menu does. That art puts the
	// composition's right edge at 748 * 2.400 = 1795. Anchoring the menu to 1920 instead lands it
	// at 1826 - 31px OUTSIDE the frame it is supposed to sit inside, and further out as the dial
	// shrinks. Reported from the running game before this comment existed.
	//
	// So the anchor point is mapped by the PER-AXIS scale, which is what the art it sits against
	// uses, while the rect's SIZE comes from the uniform scale, which is the whole point of the
	// mode. A right-hugging control then keeps the exact right edge it has today, at every dial,
	// and only its shape changes.
	//
	// A PANEL MUST NOT TEAR ALONG THIS RULE, and the anchor is therefore chosen from the GROUP's
	// edges, not from this rect's. The rule used to read them off the rect alone with the note that
	// "every control in a column shares [the edges], so a column cannot split" - true of columns,
	// false of rows, and ExtrasMenu.wnd's five label/slider pairs are the counterexample. See the
	// GROUPS block above. With no group the box IS this rect, which is that older behaviour exactly.
	//
	// A rect reaching BOTH edges anchors on its centre, as does one reaching neither: pinning centred
	// content to a side would drag half of it off the display. 20% is deliberately generous for the
	// same reason - it must take a whole column, not clip its outermost member.
	const LayoutRect &box = ( group != NULL ) ? *group : authored;

	const Int	 kAnchorPct = 20;

	const Bool nearLeft	  = ( box.loX * 100 <= ctx.createW * kAnchorPct ) ? TRUE : FALSE;
	const Bool nearRight  = ( box.hiX * 100 >= ctx.createW * ( 100 - kAnchorPct ) ) ? TRUE : FALSE;
	const Bool nearTop	  = ( box.loY * 100 <= ctx.createH * kAnchorPct ) ? TRUE : FALSE;
	const Bool nearBottom = ( box.hiY * 100 >= ctx.createH * ( 100 - kAnchorPct ) ) ? TRUE : FALSE;

	const Real anchorX = ( nearRight && !nearLeft ) ? (Real)box.hiX
										 : ( nearLeft && !nearRight ) ? (Real)box.loX
										 : ( (Real)box.loX + (Real)box.hiX ) * 0.5f;
	const Real anchorY = ( nearBottom && !nearTop ) ? (Real)box.hiY
										 : ( nearTop && !nearBottom ) ? (Real)box.loY
										 : ( (Real)box.loY + (Real)box.hiY ) * 0.5f;

	// Where the art puts that point, minus where uniform scaling would have put it. Every member of
	// a group gets the SAME origin, which is what makes the authored gap between two of them come
	// out as exactly (gap * s) - the anchor cancels.
	const Real originX = anchorX * xScale - anchorX * s;
	const Real originY = anchorY * yScale - anchorY * s;

	out->loX = (Int)( originX + (Real)authored.loX * s );
	out->loY = (Int)( originY + (Real)authored.loY * s );
	out->hiX = (Int)( originX + (Real)authored.hiX * s );
	out->hiY = (Int)( originY + (Real)authored.hiY * s );
}

/// The ungrouped map: this rect is its own group. Every caller that predates groups keeps this.
inline void LayoutScaleMapRect( const LayoutScaleContext &ctx, const LayoutRect &authored,
																LayoutRect *out )
{
	LayoutScaleMapRectGrouped( ctx, authored, NULL, out );
}
