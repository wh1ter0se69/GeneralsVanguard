// GeneralsX: S7 PREREG A44 (A23/A31): the ORDER of the early cast-only flush that captures the
// off-screen shadow casters - pure logic over its operations, host-tested (scripts/test/sm-fit/sm-fit-test.cpp A44-seq-*, with the
// compile-time mutants sm-fit-test.sh builds), used by the ZH glue (W3DShadowMap.cpp), which supplies the real operations. Render-only.
//
// The integrator's T6 conditions (17/10 22:1xZ) are part of the order: the motion-units bridge stays null across
// every Render AND both flushes; the PREVIOUS static-sort value and mesh-renderer camera come back; a frame with foreign pending work (or a
// flush-site tag already set) submits nothing and touches nothing.
#pragma once

#include <cstddef>
#include <cstdint>

namespace GXShadowOffscreen {

struct Result { uint32_t submitted = 0, dirty = 0, groups = 0; };

// Ops: bool queuesEmpty(); void* suspendMotion(); void restoreMotion(void*); void setTags(uint32_t); bool staticSort();
//      void setStaticSort(bool); void render(size_t i); void* useCasterCamera(); void restoreCamera(void*); void flushMeshes();
//      void flushSorted(); void clearOwn(); void renderGroup(size_t g); void setGroupOwn(size_t g);
// castOnlyTag: the tags held across the submission and both flushes; tagsAfter: what the scene starts with (RECEIVE).
// A61/A62 (the integrator's A61 ruling): `groups` = the own-light groups of the SORTED caster meshes (rotor discs).
// After the opaque phase, per group: its meshes Render()ed under CAST_ONLY (static sort off), the mesh flush queues them into the
// sorting renderer (containerEnd clears CAST and the own light there), then CAST_ONLY | solidTags (CAST | SOLID) and the group's own
// light hold for that group's sorting flush ALONE (the sorting renderer interleaves nodes by depth: one own light per flush), and
// CAST_ONLY comes back before anything else runs. groups == 0: today's order exactly (A62 (3)).
template <class Ops> Result run(Ops& ops, size_t n, size_t groups, uint32_t castOnlyTag, uint32_t solidTags, uint32_t tagsAfter)
{
	Result r;
	if (n == 0 && groups == 0)
		return r;
#ifdef GX_SM_SORTED_MUTANT_ALWAYSGROUP   // TEST-ONLY mutant: a group pass even when there is no sorted mesh - A61-seq-2 must FAIL
	if (groups == 0)
		groups = 1;
#endif
#ifndef GX_SM_OFFSCREEN_MUTANT_DIRTY        // TEST-ONLY mutants (sm-fit-test.sh): each must make A44-seq FAIL
	if (!ops.queuesEmpty()) {   // A31: never mixed with foreign submissions - they would be dropped under CAST_ONLY
		r.dirty = 1;
		return r;
	}
#endif
#ifndef GX_SM_OFFSCREEN_MUTANT_MOTION
	void* motion = ops.suspendMotion();   // A23: no history update, no velocity capture for a cast-only draw
#endif
	ops.setTags(castOnlyTag);
	const bool staticWas = ops.staticSort();
#ifndef GX_SM_OFFSCREEN_MUTANT_STATIC
	ops.setStaticSort(false);   // a sort-level mesh must not land on the static list the scene later DRAWS
#endif
	for (size_t i = 0; i < n; ++i)
		ops.render(i);
	ops.setStaticSort(staticWas);
	void* camera = ops.useCasterCamera();
	ops.flushMeshes();
#ifndef GX_SM_OFFSCREEN_MUTANT_SORTED
	ops.flushSorted();   // A31: the sorting queue too - nothing of the submission may wait for the scene's flush
#endif
#ifndef GX_SM_SORTED_MUTANT_ONEFLUSH
	for (size_t g = 0; g < groups; ++g) {   // A61: the sorted phase, one group at a time (CAST_ONLY on entry: containerEnd cleared CAST)
		ops.setStaticSort(false);
		ops.renderGroup(g);
		ops.setStaticSort(staticWas);
		ops.flushMeshes();
#ifndef GX_SM_SORTED_MUTANT_NOSOLID   // TEST-ONLY mutant: CAST without SOLID at the sorting flush - A61-seq-1 must FAIL
		ops.setTags(castOnlyTag | solidTags);
#else
		ops.setTags(castOnlyTag | (solidTags & ~8u));
#endif
#ifndef GX_SM_SORTED_MUTANT_NOREOWN   // TEST-ONLY mutant: the own light not re-set after containerEnd zeroed it - A61-seq-1 must FAIL
		ops.setGroupOwn(g);
#endif
#ifndef GX_SM_SORTED_MUTANT_SKIPFLUSH   // TEST-ONLY mutant: the group's sorting flush skipped - A61-seq-1 must FAIL
		ops.flushSorted();
#endif
#ifndef GX_SM_SORTED_MUTANT_SOLIDLEFT   // TEST-ONLY mutant: CAST | SOLID left set into the next group - A61-seq-1 must FAIL
		ops.setTags(castOnlyTag);
#endif
	}
#else   // TEST-ONLY mutant: every group rendered, then ONE mesh flush and ONE sorting flush (two own lights in one flush)
	if (groups > 0) {
		ops.setTags(castOnlyTag);
		ops.setStaticSort(false);
		for (size_t g = 0; g < groups; ++g)
			ops.renderGroup(g);
		ops.setStaticSort(staticWas);
		ops.flushMeshes();
		ops.setTags(castOnlyTag | solidTags);
		ops.setGroupOwn(0);
		ops.flushSorted();
		ops.setTags(castOnlyTag);
	}
#endif
	r.groups = uint32_t(groups);
#ifndef GX_SM_OFFSCREEN_MUTANT_CAMERA
	ops.restoreCamera(camera);
#else
	(void)camera;
#endif
	ops.clearOwn();
#ifndef GX_SM_OFFSCREEN_MUTANT_TAGS
	ops.setTags(tagsAfter);
#endif
#ifndef GX_SM_OFFSCREEN_MUTANT_MOTION
	ops.restoreMotion(motion);
#endif
	r.submitted = uint32_t(n);
	return r;
}

} // namespace GXShadowOffscreen
