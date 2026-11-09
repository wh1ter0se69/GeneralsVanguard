// GeneralsX @feature D · look-shadowmaps-3 17/10/2026 S7 PREREG A41: what the device must hold (tags + a clamped instance's own light) for
// each draw boundary the mesh renderer reports - pure logic, host-tested (scripts/test/sm-fit/sm-fit-test.cpp A41-tag-*), used by the ZH
// glue (W3DShadowMap.cpp), which sends each value to the device only when it changes (integrator A40 T4: on change only, no stale own light).
// Render-only.
#pragma once

#include <cstdint>

namespace GXShadowTags {

enum : uint32_t { kCast = 2u };   // GxShadowMap.h GX_SHADOW_TAG_CAST

struct Want { uint32_t tags; uint64_t own; };

// a non-skin mesh task (every pass): its root's own light (0 when the root is not a caster root or not clamped), CAST on pass 0 when the
// root casts and the mesh passes the volume code's filter
inline Want meshBegin(uint32_t tags, bool rootIn, uint64_t rootOwn, int pass, bool castFilter)
{
	Want w;
	w.own = rootIn ? rootOwn : 0;
	w.tags = (pass == 0 && rootIn && castFilter) ? (tags | kCast) : (tags & ~kCast);
	return w;
}

// a procedural or delayed material pass: the own light only (never CAST); a skin mesh has no caster copy, so none
inline Want passBegin(uint32_t tags, uint64_t held, bool skin, bool rootIn, uint64_t rootOwn)
{
	Want w;
	w.tags = tags & ~kCast;
#ifdef GX_SM_TAG_MUTANT_STALE      // TEST-ONLY mutant (sm-fit-test.sh X- build): an unclamped pass keeps the light it found
	w.own = (!skin && rootIn && rootOwn) ? rootOwn : held;
#else
	(void)held;
	w.own = (!skin && rootIn) ? rootOwn : 0;
#endif
	return w;
}

// the end of a rigid container's draws or of a material-pass loop: nothing after it inherits CAST or an own light
inline Want containerEnd(uint32_t tags)
{
	Want w = { tags & ~kCast, 0 };
	return w;
}

} // namespace GXShadowTags
