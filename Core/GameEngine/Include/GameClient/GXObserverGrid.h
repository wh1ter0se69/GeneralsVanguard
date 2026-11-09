// GeneralsX @feature Claude 02/10/2026 The observer player list's grid: where each player's button goes when a match has
// more players than ControlBar.wnd has buttons.
//
// ControlBar.wnd ships ButtonPlayer0..7 - retail lays them out as two columns of four (icon + name), ControlBar Pro as
// one column of eight icons - and a lobby now seats 14 (the roadmap wants 32). The template is measured at runtime by
// ControlBarObserver.cpp, so whichever layout is installed, and however LayoutScale mapped it, is what this extends:
//   * up to rows * cols players: the .wnd exactly, nothing moves;
//   * more: the fewest columns, rows balanced (12 -> 2x6, 14 -> 2x7, 32 -> 4x8), cells at the template's size while
//     they fit the area the observer panel may use, scaled down uniformly once they do not;
//   * and when the players are in teams, one team per column (4v4v4 -> 3x4, 7v5 -> 2x7) whenever that is as large as
//     the plain grid - the caller orders players by team, so a team never splits across columns.
// Columns run down first, like the .wnd numbers its buttons. Near-equal scales (within 2%) go to fewer columns.
//
// Header-only and pure: ControlBarObserver.cpp and scripts/test/observer-grid-test.cpp use it.

#pragma once

struct GXObserverGridTemplate
{
	int rows;			///< buttons per column in the .wnd
	int cols;			///< columns in the .wnd
	int cellW;		///< one cell: the button, plus its name label where the layout shows one
	int cellH;
	int pitchX;		///< column to column
	int pitchY;		///< row to row
	int availW;		///< from the first cell's left edge to the right edge the panel may use
	int availH;		///< from the first cell's top edge to the bottom of the .wnd's column
};

struct GXObserverGridLayout
{
	int cols;
	int rows;
	float scale;	///< applied to cell size and pitch; 1 = template size
	bool byGroup;	///< column = group, row = place in the group; otherwise item i is column i / rows, row i % rows
};

struct GXObserverGridCell
{
	int x;				///< offset of cell i from the first cell
	int y;
};

/// A template the runtime could measure as a grid. Anything else is used as authored and never divided by.
inline bool gxObserverGridValid( const GXObserverGridTemplate &t )
{
	return t.rows > 0 && t.cols > 0 && t.cellW > 0 && t.cellH > 0 && t.pitchX > 0 && t.pitchY > 0
		&& t.availW > 0 && t.availH > 0;
}

/// The scale `cols` x `rows` cells get in the area, capped at 1.
inline float gxObserverGridScale( const GXObserverGridTemplate &t, int cols, int rows )
{
	float s = 1.0f;
	const float w = (float)t.availW / (float)( ( cols - 1 ) * t.pitchX + t.cellW );
	const float h = (float)t.availH / (float)( ( rows - 1 ) * t.pitchY + t.cellH );
	if( w < s ) s = w;
	if( h < s ) s = h;
	return s;
}

inline GXObserverGridLayout gxObserverGridLayout( const GXObserverGridTemplate &t, int n )
{
	GXObserverGridLayout best;
	best.cols = t.cols;
	best.rows = t.rows;
	best.scale = 1.0f;
	best.byGroup = false;
	if( n <= t.rows * t.cols || !gxObserverGridValid( t ) )
		return best;

	best.scale = 0.0f;
	for( int cols = t.cols; cols <= n; ++cols )
	{
		const int rows = ( n + cols - 1 ) / cols;
		if( ( cols - 1 ) * rows >= n )
			continue;		// a column would be empty
		const float s = gxObserverGridScale( t, cols, rows );
		if( s > best.scale + 0.02f )
		{
			best.cols = cols;
			best.rows = rows;
			best.scale = s;
		}
		if( s >= 1.0f )
			break;			// more columns cannot beat full size
	}
	return best;
}

/// `sizes` are the players per group in display order: a team each, the players without a team as one more group.
inline GXObserverGridLayout gxObserverGridLayoutForGroups( const GXObserverGridTemplate &t, const int *sizes, int groups )
{
	int n = 0, largest = 0;
	for( int g = 0; g < groups; ++g )
	{
		n += sizes[g];
		if( sizes[g] > largest )
			largest = sizes[g];
	}
	GXObserverGridLayout plain = gxObserverGridLayout( t, n );
	if( groups < 2 || n <= t.rows * t.cols || !gxObserverGridValid( t ) )
		return plain;		// no teams, or the .wnd as authored, or a template that could not be measured

	const float s = gxObserverGridScale( t, groups, largest );
	if( s < plain.scale - 0.02f )
		return plain;
	GXObserverGridLayout l;
	l.cols = groups;
	l.rows = largest;
	l.scale = s;
	l.byGroup = true;
	return l;
}

inline GXObserverGridCell gxObserverGridCellAt( const GXObserverGridLayout &l, const GXObserverGridTemplate &t,
	int col, int row )
{
	GXObserverGridCell c;
	c.x = (int)( col * t.pitchX * l.scale + 0.5f );
	c.y = (int)( row * t.pitchY * l.scale + 0.5f );
	return c;
}

/// Item i of a plain (not byGroup) layout.
inline GXObserverGridCell gxObserverGridCell( const GXObserverGridLayout &l, const GXObserverGridTemplate &t, int i )
{
	const int rows = l.rows > 0 ? l.rows : 1;
	return gxObserverGridCellAt( l, t, i / rows, i % rows );
}
