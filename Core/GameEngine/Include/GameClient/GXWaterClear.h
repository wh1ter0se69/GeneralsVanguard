// GeneralsX: CLEARER WATER, a client-only look option (look S8a, 5.1; wh1ter0se 13/10:
// "actual water ... invisible/white water compared to the blue one"). It is ON by default from 0.9.66 (gxWaterClearOptionOn
// below): Options.ini GXWaterClear = no turns it off; GX_WATER_CLEAR=0/1 overrides the key for A/B arms.
//
// HOW RETAIL DRAWS STANDING WATER. The frame's destination alpha is cleared to TransparentWaterMinOpacity (M) every frame
// (W3DDisplay::draw's Begin_Render, W3DView, W3DShaderManager). Terrain tiles near a shore - every tile with a vertex less
// than TransparentWaterDepth * M under the surface - are drawn again into destination alpha only, through a 256-texel LUT
// (BaseHeightMap::updateShorelineTile, initDestAlphaLUT): texel t = depth / (D * M), alpha = min(t, M). The water polygons
// are then blended with SRC = DESTALPHA, DEST = INVDESTALPHA (W3DWater.cpp drawTrapezoidWater). So the water's opacity
// rises from 0 at the shore line with slope 1 / (D * M) and stops at M, which it reaches at depth D * M * M - NOT at D, as
// the PLAN assumed. Deep water is M opaque: stock M is 1, so the sea floor is never seen.
//
// WHAT THIS CHANGES. Two Water values, as one more override on TheWaterTransparency's chain, after the map's own map.ini
// blocks: M falls toward kGxWaterClearMinOpacity (deep water shows the sea floor through it), and D grows so the fade ends
// kGxWaterClearReach times further out than the map's own fade did. In D * M * M terms, so the reach survives the lower M:
// "D x 1.6" alone, at M 0.6, would END the fade at 1.6 * 0.36 = 0.58 of today's distance - shorter, not longer. Rivers are
// untouched: drawRiverWater blends with its own vertex alpha and never reads destination alpha. Nothing else changes: the
// colours, the textures, the sky names (W3DWater.cpp mapChoseOwnSky compares those), the radar colour are copied.
//
// RENDER-ONLY. Every reader of these two values is render code (BaseHeightMap, W3DWater, W3DWaterTracks, W3DShaderManager,
// W3DView, W3DDisplay); the simulation never reads TheWaterTransparency (GameLogic::reset only deletes the map's overrides).
// This header is pure logic with no engine include and sits in Core/GameEngine/Include/GameClient, outside the SimID digest
// (resources/gitinfo/simsourcedigest_watcher.cmake); never include it from Common/, GameLogic/ or GameNetwork/. The engine
// glue is W3DDevice/GameClient/Water/GXWaterClearGlue.inl, outside every GX-SIMID region.
//
// The engine-free functions below are what scripts/test/look-water-clear-test.sh checks.

#pragma once

#include <stdlib.h>

// ---------------------------------------------------------------------------------------------------------------------------
// The look at strength 1. Starting values from the look director; pictures tune them, and the GX_WATER_CLEAR_*
// variables below override them per run without a rebuild.

static constexpr float kGxWaterClearMinOpacity = 0.6f;	// deep-water opacity cap (stock Water.ini: 1.0, opaque)
static constexpr float kGxWaterClearReach = 1.6f;		// the shore fade ends this many times further out than the map's own
static constexpr float kGxWaterClearStrength = 1.0f;	// 0 = the map's own water exactly, 1 = the targets above

// What the knobs accept. A reach below 1 shortens the fade (0.58 is "D x 1.6" read literally at M 0.6).
static constexpr float kGxWaterClearMinOpacityLo = 0.05f;
static constexpr float kGxWaterClearReachLo = 0.1f;
static constexpr float kGxWaterClearReachHi = 8.0f;

struct GxWaterClearTargets
{
	float minOpacity;
	float reach;
};

struct GxWaterClearValues
{
	float depth;		// TransparentWaterDepth
	float minOpacity;	// TransparentWaterMinOpacity
};

inline GxWaterClearTargets gxWaterClearDefaultTargets()
{
	GxWaterClearTargets t = { kGxWaterClearMinOpacity, kGxWaterClearReach };
	return t;
}

inline bool gxWaterClearFinite(float v)
{
	return v == v && v - v == 0.0f;	// false for NaN and both infinities
}

// v clamped to [lo, hi]; fallback when v is not a number.
inline float gxWaterClearClamp(float v, float fallback, float lo, float hi)
{
	if (!(v == v))
		return fallback;
	if (v < lo)
		return lo;
	if (v > hi)
		return hi;
	return v;
}

// The depth at which the shore fade reaches its cap: D * M * M (see the top of this file).
inline float gxWaterClearFadeEnd(float depth, float minOpacity)
{
	return depth * minOpacity * minOpacity;
}

// The two values the override carries, from the values it overrides (the map's own, or Water.ini's when the map has none).
// Strength 0 or less, a map that turned the soft edge off (depth 0: no shoreline tiles, no destination-alpha blend) and a
// degenerate or non-finite input all return the input unchanged, so "off" is the map's own water bit for bit. The opacity is
// never raised: a map already clearer than the target keeps its own M and only gets the longer fade.
inline GxWaterClearValues gxWaterClearApply(float depth, float minOpacity, float strength, GxWaterClearTargets t)
{
	GxWaterClearValues out = { depth, minOpacity };
	if (!(strength > 0.0f) || !gxWaterClearFinite(strength) || !gxWaterClearFinite(depth) || !gxWaterClearFinite(minOpacity)
		|| depth <= 0.0f || minOpacity <= 0.0f)
		return out;
	if (strength > 1.0f)
		strength = 1.0f;

	const float target = gxWaterClearClamp(t.minOpacity, kGxWaterClearMinOpacity, kGxWaterClearMinOpacityLo, 1.0f);
	const float reachTarget = gxWaterClearClamp(t.reach, kGxWaterClearReach, kGxWaterClearReachLo, kGxWaterClearReachHi);

	float m = minOpacity;
	if (target < m)
		m = minOpacity + strength * (target - minOpacity);
	const float reach = 1.0f + strength * (reachTarget - 1.0f);
	const float fadeEnd = gxWaterClearFadeEnd(depth, minOpacity) * reach;

	out.minOpacity = m;
	out.depth = fadeEnd / (m * m);
	return out;
}

// ---------------------------------------------------------------------------------------------------------------------------
// The Options.ini key. GeneralsX: ON BY DEFAULT from 0.9.66 (for wh1ter0se, who chose this water
// from the look-water-3 pictures, where clearer and live water were on together): an Options.ini without GXWaterClear draws
// clearer water, and GXWaterClear = no turns it off.
static const bool kGxWaterClearDefaultOn = true;

// The key's value read exactly as UserPreferences::getBool reads it (UserPreferences.cpp:193): missing or empty ->
// kGxWaterClearDefaultOn; otherwise, ignoring case, "1", "t", "true", "y", "yes" or "ok" is on and anything else ("no" too) off.
inline bool gxWaterClearOptionOn(const char *value)
{
	if (value == nullptr || value[0] == '\0')
		return kGxWaterClearDefaultOn;
	static const char *const kTrue[] = { "1", "t", "true", "y", "yes", "ok" };
	for (const char *w : kTrue)
	{
		size_t i = 0;
		for (; w[i] != '\0'; ++i)
		{
			char c = value[i];
			if (c >= 'A' && c <= 'Z')
				c = (char)(c - 'A' + 'a');
			if (c != w[i])
				break;
		}
		if (w[i] == '\0' && value[i] == '\0')
			return true;
	}
	return false;
}

// ---------------------------------------------------------------------------------------------------------------------------
// Parsing the A/B variables. Pure, so they are tested too.

// GX_WATER_CLEAR: 1 (or y/t) on, 0 (or n/f) off, -1 when unset or unreadable (the Options.ini key then decides).
inline int gxWaterClearParseBool(const char *s)
{
	if (s == nullptr)
		return -1;
	while (*s == ' ' || *s == '\t')
		++s;
	switch (*s)
	{
		case '0': case 'n': case 'N': case 'f': case 'F':
			return 0;
		case '1': case 'y': case 'Y': case 't': case 'T':
			return 1;
		default:
			return -1;
	}
}

// GX_WATER_CLEAR_STRENGTH / _MINOPACITY / _REACH: a whole number, clamped to [lo, hi]; fallback when unset, empty or junk.
inline float gxWaterClearParseReal(const char *s, float fallback, float lo, float hi)
{
	if (s == nullptr || *s == 0)
		return fallback;
	char *end = nullptr;
	const double v = strtod(s, &end);
	if (end == s)
		return fallback;
	while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')
		++end;
	if (*end != 0)
		return fallback;
	return gxWaterClearClamp((float)v, fallback, lo, hi);
}

// GX_WATER_CLEAR_FLIP_AT=900,1800,... (test only): the logic frames, ascending, at which the option flips inside each loaded
// map, so one picture run shows ON -> OFF -> ON over the same map. The list ends at the first entry that is not a number
// greater than the one before it.
enum { GXWATER_MAX_FLIPS = 8 };

struct GxWaterClearFlips
{
	int n;
	unsigned frames[ GXWATER_MAX_FLIPS ];
};

inline GxWaterClearFlips gxWaterClearParseFlips(const char *s)
{
	GxWaterClearFlips f;
	f.n = 0;
	while (s != nullptr && *s != 0 && f.n < GXWATER_MAX_FLIPS)
	{
		char *end = nullptr;
		const unsigned long v = strtoul(s, &end, 10);
		if (end == s || (f.n > 0 && v <= f.frames[ f.n - 1 ]) || v > 0x7fffffffUL)
			break;
		f.frames[ f.n++ ] = (unsigned)v;
		if (*end != ',')
			break;
		s = end + 1;
	}
	return f;
}

// How many flips are due at logic frame `frame`, counting from `next` (the first not yet taken); returns the new `next`.
// Each one crossed flips the option once, so a frame jump past two of them leaves it as it was.
inline int gxWaterClearFlipsDue(const GxWaterClearFlips &f, int next, unsigned frame)
{
	while (next >= 0 && next < f.n && frame >= f.frames[ next ])
		++next;
	return next;
}

// ---------------------------------------------------------------------------------------------------------------------------
// The override's place in an Overridable chain, written against the chain's own interface (friend_getNextOverride,
// setNextOverride, friend_restoreOverrideLink) so the test can drive it with a stand-in node.

// The node whose next is `ours`, walking from `head`; nullptr when `ours` is not in the chain (or is the head itself).
template <class Node>
Node *gxWaterClearPredecessor(Node *head, const Node *ours)
{
	if (head == nullptr || ours == nullptr)
		return nullptr;
	for (Node *n = head; n != nullptr; n = (Node *)n->friend_getNextOverride())
	{
		if ((const Node *)n->friend_getNextOverride() == ours)
			return n;
	}
	return nullptr;
}

// Takes `ours` out of the chain whose node before it is `pred`, keeping whatever followed it, and leaves `ours` linked to
// nothing so deleting it frees only itself (Overridable's destructor deletes its next).
template <class Node>
void gxWaterClearUnlink(Node *pred, Node *ours)
{
	Node *after = (Node *)ours->friend_getNextOverride();
	pred->friend_restoreOverrideLink(after, pred->isOverride());
	ours->friend_restoreOverrideLink(nullptr, true);
}

// ---------------------------------------------------------------------------------------------------------------------------
// The engine entry points (GXWaterClearGlue.inl). Plain types, so this header stays engine-free.

// After every map load (WaterRenderObjClass::load, the end of W3DTerrainVisual::load): re-reads Options.ini and the
// variables, arms the flip list, applies, refreshes. Also what a persisted ON at startup rides: the shell map is a map load.
void gxWaterClearMapLoaded();
// When the map is torn down (skyBoxMapReset, W3DDisplay::reset - GameLogic::reset has just deleted the override).
void gxWaterClearMapReset();
// Once per client frame (WaterRenderObjClass::update): the flip list.
void gxWaterClearPoll();
// A runtime change (a menu, the look governor): applies at once inside a loaded map and lasts until that map ends. Not
// persisted, and outside a loaded map it changes nothing lasting: every map load re-reads Options.ini and the variables,
// so a caller that wants the change kept writes the key itself first.
void gxWaterClearSet(bool on, float strength);
