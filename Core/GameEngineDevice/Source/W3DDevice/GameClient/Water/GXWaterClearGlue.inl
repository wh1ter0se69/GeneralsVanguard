// GeneralsX: CLEARER WATER - the engine glue (the look and its arithmetic: GameClient/GXWaterClear.h).
//
// Included at the end of W3DWater.cpp, below every GX-SIMID region, and called from three places that are outside the SimID
// closure: WaterRenderObjClass::load and ::update (simdigest_exclusions.txt: the water object's own load and per-frame update)
// and skyBoxMapReset (W3DDisplay::reset, also on that list). It writes TheWaterTransparency's override chain and, through
// BaseHeightMapRenderObjClass::setShoreLineDetail, the terrain's shoreline tiles and depth LUT; only render code reads either.
//
// THE OVERRIDE. One WaterTransparencySetting appended LAST on TheWaterTransparency's chain - after Water.ini's master and the
// map's own map.ini / solo.ini blocks, which loadMapINI appends before the terrain loads - holding a copy of the final values
// with the two the option moves. It is an override (markAsOverride), so GameLogic::reset deletes it with the map's own; the
// pointer kept here is dropped at that teardown (skyBoxMapReset) and never dereferenced unless it is found on the live chain.
// OFF unlinks and deletes it: the chain is then exactly Water.ini plus the map's own again.
//
// THE REFRESH. setShoreLineDetail rescans the whole map's shoreline tiles from the chain and refreshes the depth LUT when the
// opacity moved (BaseHeightMap.cpp). The other readers (the frame's alpha clear, the water's blend) read the chain every frame.
// No texture moves: the copy keeps every texture name, so updateMapOverrides has nothing to re-point.

#include "Common/OptionPreferences.h"

namespace
{
	WaterTransparencySetting *s_gxWaterNode = nullptr;	// our override, while one is on the chain
	Bool s_gxWaterMapLoaded = FALSE;					// from WaterRenderObjClass::load to the map's teardown
	Bool s_gxWaterOn = FALSE;
	Real s_gxWaterStrength = kGxWaterClearStrength;
	GxWaterClearTargets s_gxWaterTargets = { kGxWaterClearMinOpacity, kGxWaterClearReach };
	GxWaterClearFlips s_gxWaterFlips = { 0, { 0 } };
	Int s_gxWaterNextFlip = 0;
	const char *s_gxWaterSource = "default";
}

// Options.ini's GXWaterClear / GXWaterClearStrength, then the A/B variables over them. Every map load, so a key edited between
// matches counts from the next map.
static void gxWaterClearReadSettings()
{
	OptionPreferences prefs;
	const OptionPreferences::const_iterator key = prefs.find("GXWaterClear");	// absent = ON from 0.9.66 (GXWaterClear.h)
	s_gxWaterOn = gxWaterClearOptionOn((key != prefs.end()) ? key->second.str() : nullptr);
	s_gxWaterSource = (key != prefs.end()) ? "Options.ini" : "default";
	s_gxWaterStrength = gxWaterClearClamp(prefs.getReal("GXWaterClearStrength", kGxWaterClearStrength), kGxWaterClearStrength, 0.0f, 1.0f);

	const Int env = gxWaterClearParseBool(getenv("GX_WATER_CLEAR"));
	if (env >= 0)
	{
		s_gxWaterOn = (env == 1);
		s_gxWaterSource = "GX_WATER_CLEAR";
	}
	s_gxWaterStrength = gxWaterClearParseReal(getenv("GX_WATER_CLEAR_STRENGTH"), s_gxWaterStrength, 0.0f, 1.0f);
	s_gxWaterTargets.minOpacity = gxWaterClearParseReal(getenv("GX_WATER_CLEAR_MINOPACITY"), kGxWaterClearMinOpacity,
		kGxWaterClearMinOpacityLo, 1.0f);
	s_gxWaterTargets.reach = gxWaterClearParseReal(getenv("GX_WATER_CLEAR_REACH"), kGxWaterClearReach,
		kGxWaterClearReachLo, kGxWaterClearReachHi);
	s_gxWaterFlips = gxWaterClearParseFlips(getenv("GX_WATER_CLEAR_FLIP_AT"));
	s_gxWaterNextFlip = 0;
}

// Puts the chain where the option says, refreshes what caches it, and says so in one line.
static void gxWaterClearApplyNow(const char *why)
{
	if (TheWaterTransparency == nullptr)
		return;
	WaterTransparencySetting *master = (WaterTransparencySetting *)TheWaterTransparency.getNonOverloadedPointer();
	Overridable *head = master;

	// Ours is only ever touched once found on the live chain.
	Overridable *pred = gxWaterClearPredecessor(head, (const Overridable *)s_gxWaterNode);
	if (s_gxWaterNode != nullptr && pred == nullptr)
	{
		fprintf(stderr, "[GXWATER] the override is no longer on the chain - forgotten, not touched\n");
		s_gxWaterNode = nullptr;
	}
	if (s_gxWaterNode != nullptr && s_gxWaterNode->friend_getNextOverride() != nullptr)
	{
		// Nothing appends after ours today (map INIs load after the reset that deleted it); if something did, ours comes
		// out here and goes back on at the end below, and the later block keeps whatever it copied.
		fprintf(stderr, "[GXWATER] an override follows ours - taking ours out first\n");
		gxWaterClearUnlink(pred, (Overridable *)s_gxWaterNode);
		deleteInstance(s_gxWaterNode);
		s_gxWaterNode = nullptr;
		pred = nullptr;
	}

	// The values the option starts from: the node before ours, or the chain's last when ours is absent.
	const WaterTransparencySetting *own = (s_gxWaterNode != nullptr)
		? (const WaterTransparencySetting *)pred
		: (const WaterTransparencySetting *)head->friend_getFinalOverride();
	GxWaterClearValues v = gxWaterClearApply(own->m_transparentWaterDepth, own->m_minWaterOpacity,
		s_gxWaterOn ? s_gxWaterStrength : 0.0f, s_gxWaterTargets);
	// GeneralsX: RA3 water, while on, supplies both values itself (its longer Beer-Lambert band and
	// its own cap, GameClient/GXWaterRA3.h), whatever this option's key says; off, this returns false and nothing changes.
	const Bool ra3 = gxWaterRA3OverrideValues(own->m_transparentWaterDepth, own->m_minWaterOpacity, &v.depth, &v.minOpacity) ? TRUE : FALSE;
	const Bool wanted = (v.depth != own->m_transparentWaterDepth || v.minOpacity != own->m_minWaterOpacity);
	const Int tilesBefore = (TheTerrainRenderObject != nullptr) ? TheTerrainRenderObject->getNumShoreLineTiles(FALSE) : -1;

	Bool moved = FALSE;
	if (wanted)
	{
		if (s_gxWaterNode == nullptr)
		{
			WaterTransparencySetting *last = (WaterTransparencySetting *)head->friend_getFinalOverride();
			s_gxWaterNode = newInstance(WaterTransparencySetting);
			*s_gxWaterNode = *last;	// every field the map (or Water.ini) chose, as INIWater.cpp's own overrides copy them
			s_gxWaterNode->friend_restoreOverrideLink(nullptr, TRUE);	// an override, and the chain's last
			last->setNextOverride(s_gxWaterNode);
			moved = TRUE;
		}
		if (s_gxWaterNode->m_transparentWaterDepth != v.depth || s_gxWaterNode->m_minWaterOpacity != v.minOpacity)
			moved = TRUE;
		s_gxWaterNode->m_transparentWaterDepth = v.depth;
		s_gxWaterNode->m_minWaterOpacity = v.minOpacity;
	}
	else if (s_gxWaterNode != nullptr)
	{
		gxWaterClearUnlink(pred, (Overridable *)s_gxWaterNode);
		deleteInstance(s_gxWaterNode);
		s_gxWaterNode = nullptr;
		moved = TRUE;
	}

	// Both directions: the tiles and the LUT follow the chain as it is now. Nothing moved, nothing is touched - an OFF map load
	// leaves the terrain exactly as W3DTerrainVisual::load built it.
	if (moved && TheTerrainRenderObject != nullptr)
		TheTerrainRenderObject->setShoreLineDetail();

	const WaterTransparencySetting *now = TheWaterTransparency;
	fprintf(stderr, "[GXWATER] clear %s minOpacity=%.3f depth=%.3f (map %.3f/%.3f) applied=%d strength=%.2f target=%.2f/%.2f"
		" tiles=%d->%d src=%s why=%s\n",
		ra3 ? "ra3" : s_gxWaterOn ? "on" : "off", now->m_minWaterOpacity, now->m_transparentWaterDepth, own->m_minWaterOpacity,
		own->m_transparentWaterDepth, (s_gxWaterNode != nullptr) ? 1 : 0, s_gxWaterStrength, s_gxWaterTargets.minOpacity,
		s_gxWaterTargets.reach, tilesBefore,
		(TheTerrainRenderObject != nullptr) ? TheTerrainRenderObject->getNumShoreLineTiles(FALSE) : -1, s_gxWaterSource, why);
}

void gxWaterClearMapLoaded()
{
	if (TheGlobalData != nullptr && TheGlobalData->m_isWorldBuilder)
		return;
	gxWaterClearReadSettings();
	s_gxWaterMapLoaded = TRUE;
	gxWaterClearApplyNow("map");
}

void gxWaterClearMapReset()
{
	// GameLogic::reset (GameEngine::resetSubsystems, just before the subsystems' resets that reach here) deleted ours with the
	// map's own overrides. Kept only if it is still on the chain - a display reset without a logic reset - else dropped
	// unread: the pool may hand its address to the next map's override.
	if (s_gxWaterNode != nullptr && TheWaterTransparency != nullptr)
	{
		Overridable *head = (WaterTransparencySetting *)TheWaterTransparency.getNonOverloadedPointer();
		if (gxWaterClearPredecessor(head, (const Overridable *)s_gxWaterNode) != nullptr)
			return;
	}
	s_gxWaterNode = nullptr;
	s_gxWaterMapLoaded = FALSE;
	s_gxWaterNextFlip = 0;
}

void gxWaterClearPoll()
{
	if (!s_gxWaterMapLoaded || s_gxWaterFlips.n == 0 || TheGameLogic == nullptr)
		return;
	const Int next = gxWaterClearFlipsDue(s_gxWaterFlips, s_gxWaterNextFlip, TheGameLogic->getFrame());
	if (next == s_gxWaterNextFlip)
		return;
	if ((next - s_gxWaterNextFlip) & 1)
		s_gxWaterOn = !s_gxWaterOn;
	s_gxWaterNextFlip = next;
	gxWaterClearApplyNow("flip");
}

void gxWaterClearSet(bool on, float strength)
{
	s_gxWaterOn = on ? TRUE : FALSE;
	s_gxWaterStrength = gxWaterClearClamp(strength, kGxWaterClearStrength, 0.0f, 1.0f);
	if (s_gxWaterMapLoaded)
		gxWaterClearApplyNow("set");
}
