// GeneralsX: The REMASTERED ground's switch and quality steps (D · look-director-2, slice 1 of
// look-director/HANDBACK_look-ground-2.md §5; wh1ter0se 14/10 "new ground looks better" = the direction, wh1ter0se 17/10: every look
// feature ships with 2-4 quality steps a runtime governor can move).
//
// THE GROUND. W3DDevice/GameClient/TerrainTex.cpp draws EA's 2003 terrain tiles with height-aware edges, a baked sun relief
// (BASE, replacing the retail terrain pixel shader) and a second MULT pass (the stock cloud and macro noise, a world detail
// grain and a world macro variation). It is ON by default (wh1ter0se 17/10, "yes all green" to the look set: kGxGroundKeyDefault
// below); Options.ini `GXGround = no` turns it off. GX_GROUND overrides the key for A/B arms: `off`, or "shade,blend,detail,macro,noise" in percent (1-5 values, the
// rest 100; the key's values are 100,100,100,100,50). Off draws today's ground byte for byte: nothing is built, nothing drawn.
//
// THE STEPS, one per drawn frame:
//   0 = full: BASE + MULT with every layer;
//   1 = MULT without the detail grain and the macro variation (a 2-texture program instead of 4); BASE's relief and edges stay;
//   2 = the stock ground (the retail terrain shader, no second pass).
// The look governor (GameClient/GXLookGovernor.h, knob GXK_GROUND) writes g_gxGroundGovStep after a frame commits; nothing else
// does. A step change allocates nothing: every device object and the shade atlas stay built, so a raise draws at once.
//
// Header-only and pure (no engine include), so the rules below are tested without the engine: scripts/test/look-ground-test.sh.
// Core/GameEngine/Include/GameClient is outside the SimID digest. It must stay that way: never include this from Common/,
// GameLogic/ or GameNetwork/.

#pragma once

// The Options.ini GXGround key's value when the key is absent (TerrainTex.cpp gxgDecideForMap, its only reader). wh1ter0se 17/10: the
// REMASTERED ground for everyone; the look governor may still drop its steps in a big fight.
static const bool kGxGroundKeyDefault = true;

enum
{
	GXGROUND_STEP_FULL = 0,
	GXGROUND_STEP_NO_LAYERS = 1,	// no detail grain, no macro variation
	GXGROUND_STEP_STOCK = 2,
	GXGROUND_STEPS = 3
};

// Written by the look governor's glue only (W3DDisplay.cpp gxGovPublish); 0 while it is off.
inline int g_gxGroundGovStep = GXGROUND_STEP_FULL;

// TRUE while the REMASTERED ground is really drawn (on for this map, and its shade atlas baked): written by TerrainTex.cpp only. The
// governor reads it so it never spends a step on a ground the player does not have.
inline bool g_gxGroundEngaged = false;

// TRUE once a terrain base pass offered the ground its path - the one-pass retail pixel-shader pass on a ps.1.1 chipset, which is the
// only pass GXGround_Base replaces (HeightMap.cpp) - since the governor glue last read it (W3DDisplay.cpp, which clears it). The
// multi-pass fallback (missing terrain .pso files) and a chipset without ps.1.1 never set it, so the governor does not spend a step,
// and its hold periods, on a ground those paths never draw. Set whatever the governor's own step is.
inline bool g_gxGroundBasePath = false;

inline int gxGroundClampStep( int s )
{
	return s < 0 ? GXGROUND_STEP_FULL : ( s >= GXGROUND_STEPS ? GXGROUND_STEPS - 1 : s );
}

inline const char *gxGroundStepName( int s )
{
	static const char *const kNames[ GXGROUND_STEPS ] = { "full", "no-detail-macro", "stock" };
	return kNames[ gxGroundClampStep( s ) ];
}

// "a,b,c": 1 to n plain decimal integers (optional spaces before each, an optional leading '-'), comma separated, nothing else.
// Returns how many were read; 0 = not such a list (refused - out may hold part of it, discard it), so a typo is never silently
// read as a preset.
inline int gxGroundParseInts( const char *s, int *out, int n )
{
	if( s == nullptr || *s == '\0' )
		return 0;
	int got = 0;
	while( true )
	{
		while( *s == ' ' )
			++s;
		bool neg = false;
		if( *s == '-' )
		{
			neg = true;
			++s;
		}
		if( *s < '0' || *s > '9' || got >= n )
			return 0;
		long v = 0;
		while( *s >= '0' && *s <= '9' )
		{
			if( v < 100000 )
				v = v * 10 + ( *s - '0' );
			++s;
		}
		out[ got++ ] = (int)( neg ? -v : v );
		if( *s == '\0' )
			return got;
		if( *s != ',' )
			return 0;
		++s;
	}
}

// What GX_GROUND says.
enum GxGroundEnv
{
	GXGROUND_ENV_NONE,		// unset or empty: the key decides
	GXGROUND_ENV_OFF,		// "off": off whatever the key says
	GXGROUND_ENV_LIST,		// a preset list: on with those values
	GXGROUND_ENV_BAD		// anything else: refused (logged), the key decides
};

inline int gxGroundEnvState( const char *env )
{
	if( env == nullptr || *env == '\0' )
		return GXGROUND_ENV_NONE;
	if( ( env[ 0 ] == 'o' || env[ 0 ] == 'O' ) && ( env[ 1 ] == 'f' || env[ 1 ] == 'F' ) && ( env[ 2 ] == 'f' || env[ 2 ] == 'F' )
		&& env[ 3 ] == '\0' )
		return GXGROUND_ENV_OFF;
	int v[ 5 ];
	return gxGroundParseInts( env, v, 5 ) > 0 ? GXGROUND_ENV_LIST : GXGROUND_ENV_BAD;
}

// This map's decision, and why. The order is the rule: GX_GROUND=off wins; then something must ask for the ground (a GX_GROUND
// list, or the key); then one veto - a -headless peer has no renderer. The native renderer draws the ground like classic: its
// three hand-assembled programs are catalogued (gx:ground-base / -mult / -mult-lite, GXRender/Pipeline/GxCustomCatalog.h; R-G1,
// E · water-live-native-port 21/10/2026, which retired the native veto). Changing their tokens (GameClient/GXGroundPrograms.h) needs the
// catalogue regenerated (scripts/render/ws-E/gen_catalog.py) - gx_cs_test fails until it is, and a native device REFUSES unknown programs.
enum GxGroundWhy
{
	GXGROUND_OFF_KEY,			// nothing asked: Options.ini GXGround = no (absent reads as kGxGroundKeyDefault, ON)
	GXGROUND_OFF_ENV,			// GX_GROUND=off
	GXGROUND_OFF_HEADLESS,		// asked, but -headless
	GXGROUND_ON_ENV,			// GX_GROUND=<list>
	GXGROUND_ON_KEY				// Options.ini GXGround = yes, or absent (the default)
};

inline bool gxGroundIsOn( int why )
{
	return why == GXGROUND_ON_ENV || why == GXGROUND_ON_KEY;
}

// envState = gxGroundEnvState(GX_GROUND); key = Options.ini GXGround (only consulted when GX_GROUND names no list and no off).
inline int gxGroundDecide( bool headless, int envState, bool key )
{
	if( envState == GXGROUND_ENV_OFF )
		return GXGROUND_OFF_ENV;
	const bool byEnv = envState == GXGROUND_ENV_LIST;
	if( !byEnv && !key )
		return GXGROUND_OFF_KEY;
	if( headless )
		return GXGROUND_OFF_HEADLESS;
	return byEnv ? GXGROUND_ON_ENV : GXGROUND_ON_KEY;
}
