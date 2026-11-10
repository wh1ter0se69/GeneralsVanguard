// GeneralsX: THE RENDERER SWITCH (wh1ter0se 21/10: the native renderer ships OPT-IN in 0.9.67,
// classic stays the default; native as the default is a 0.9.68 decision). The decision only - pure, no engine, unit-tested by
// scripts/test/renderer-key-test.sh. Its one reader is GXGraphics::nativeRendererRequested (GXGraphicsSettings.cpp).
//
// Options.ini `GXRenderer = classic | native`, any case. Absent = kGxRendererKeyDefault (classic). Any other value = classic, and
// the decision says so (the one [GXRENDERER] line). GX_RENDERER (TEST-ONLY: harness arms) wins over the key when it is set and not
// empty, read exactly as the native renderer always read it (GxNativeD3D8.cpp GxNativeRequested): "native" = native, any other
// value = classic.
//
// This is the ASK, not the outcome: a native session can still go classic at device creation (rows S2-S10, a "[GXNATIVE]
// session=classic reason=" line), and a build without GX_RENDER_NATIVE is classic whatever this says (GxNativeInstallOff.cpp).
// Render-only: nothing the simulation reads may ever depend on it.
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

// What a player who never chose gets. The 0.9.68 flip, if wh1ter0se says so, is this line.
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
