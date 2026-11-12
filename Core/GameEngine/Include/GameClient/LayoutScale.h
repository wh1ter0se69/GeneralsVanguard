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
** Maps a .wnd's authored coordinates to screen pixels, as a pure function of six integers.
** STRETCH scales each axis by its own ratio (a 4:3 layout on 16:9 comes out 33% wider).
** UNIFORM uses one factor for both axes, keeping shapes, and anchors each GROUP of controls to its art's edge.
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
** Both the rect path and the parser's font path must clamp: a font path that multiplied by the
** raw value would give GX_UI_SCALE=0.01 boxes at 0.05 and text at 0.01 - text floored to 1pt inside
** boxes twenty times too big for it. Two copies of a clamp is one copy too many.
*/
inline Real LayoutScaleClampDial( Real dial )
{
	return ( dial < 0.05f ) ? 0.05f : dial;
}

/**
** Does this window cover the whole authored canvas? Such layout roots and backdrops must keep filling the display.
** All four edges are tested: size alone does not prove canvas-aligned ({-100,0,700,600} is 800 wide on 800x600).
** 1% tolerance: LanGameOptionsMenu.wnd's root is 0,0..799,599 on 800x600, MainMenu.wnd's is 0,0..800,600.
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

/* GROUPS: controls sharing an authored row (loY,hiY) or column (loX,hiX), transitively, share one anchor.
** Rects keep their authored gap only if they share one anchor point (ExtrasMenu.wnd label/slider pairs tore).
** Equality is transitive, so the partition ignores arrival order; a tolerance band would chain across the screen.
** A first pass over SCREENRECT lines builds the table, as parent/sibling links do not exist at map time. */

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

	// Anchor point: mapped by the per-axis scale the backdrop art uses. Rect SIZE: the uniform scale.
	// Centring or edge-anchoring instead leaves right-hugging menus inboard of, or outside, the art's frame.
	// The anchor comes from the GROUP's edges so a panel cannot tear; with no group the box is this rect.
	// A rect reaching both edges or neither anchors on its centre; 20% is generous so a whole column is taken.
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
