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

// THE RENDERER SWITCH: the pure decision; its one reader is GXGraphics::nativeRendererRequested.
// Options.ini `GXRenderer = classic | native`, any case; absent or any other value = classic.
// GX_RENDERER (test-only) wins when set and non-empty: "native" = native, anything else = classic.
// This is the ASK, not the outcome: a native session can still go classic at device creation. Render-only.
#pragma once

enum GxRendererPick
{
	GXRENDERER_CLASSIC = 0,
	GXRENDERER_NATIVE = 1
};

enum GxRendererSource
{
	GXRENDERER_SRC_DEFAULT = 0,		// no key, no env: kGxRendererKeyDefault
	GXRENDERER_SRC_KEY,				// Options.ini GXRenderer
	GXRENDERER_SRC_ENV				// GX_RENDERER
};

// What a player who never chose gets. Changing the default is this one line.
constexpr int kGxRendererKeyDefault = GXRENDERER_CLASSIC;

struct GxRendererDecision
{
	int pick;		// GxRendererPick
	int source;		// GxRendererSource: the input that decided
	bool bad;		// that input held neither word (it decided classic)
};

// v equals the lower-case word, ASCII case folded when foldCase.
inline bool gxRendererWordIs( const char *v, const char *word, bool foldCase )
{
	for( ; *word != '\0'; ++v, ++word )
	{
		char c = *v;
		if( foldCase && c >= 'A' && c <= 'Z' )
			c = char( c - 'A' + 'a' );
		if( c != *word )
			return false;	// also where v ends first: its '\0' never equals a letter of the word
	}
	return *v == '\0';
}

// env = getenv("GX_RENDERER"); key = Options.ini GXRenderer's value, null when the file has no such line.
inline GxRendererDecision gxRendererDecide( const char *env, const char *key )
{
	GxRendererDecision d = { kGxRendererKeyDefault, GXRENDERER_SRC_DEFAULT, false };
	if( env != nullptr && env[ 0 ] != '\0' )
	{
		d.source = GXRENDERER_SRC_ENV;
		d.pick = gxRendererWordIs( env, "native", false ) ? GXRENDERER_NATIVE : GXRENDERER_CLASSIC;
		d.bad = d.pick == GXRENDERER_CLASSIC && !gxRendererWordIs( env, "classic", false );
		return d;
	}
	if( key != nullptr )
	{
		d.source = GXRENDERER_SRC_KEY;
		if( gxRendererWordIs( key, "native", true ) )
			d.pick = GXRENDERER_NATIVE;
		else
		{
			d.pick = GXRENDERER_CLASSIC;
			d.bad = !gxRendererWordIs( key, "classic", true );
		}
	}
	return d;
}

inline const char *gxRendererName( int pick )
{
	return pick == GXRENDERER_NATIVE ? "native" : "classic";
}

inline const char *gxRendererSourceName( int source )
{
	return source == GXRENDERER_SRC_ENV ? "GX_RENDERER" : source == GXRENDERER_SRC_KEY ? "Options.ini" : "default";
}
