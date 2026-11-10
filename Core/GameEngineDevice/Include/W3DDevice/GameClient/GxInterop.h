// GeneralsX: WS-X hybrid spike (render-seam/DESIGN_hybrid.md): external Vulkan recording
// inside DXVK's own device, inserted after the L2 producer flushes of RTS3DScene.
//
// GX_RENDER_HYBRID = 0 (default: nothing runs, no QueryInterface, no Vulkan call, no flush hook), 1 = P-L (record inside DXVK's
// command list through the fork's IDxvkGxExternal), 2 = P-Q (the stock interop queue protocol: flush, own submit under
// the submission lock). Each insertion records one rendering instance on the bound colour + depth/stencil with LOAD/STORE
// and one instanced test draw; GX_RENDER_HYBRID_VISIBLE=1 shows it, otherwise colour/depth writes are masked so the
// picture must stay identical to 0. Render-only: nothing here is read by the simulation.
#pragma once

namespace GxInterop
{
	enum Site
	{
		SITE_OPAQUE,			// RTS3DScene::Flush, all non-translucent objects
		SITE_OCCL_TRANSL,		// flushOccludedObjectsIntoStencil: around each translucent occludee (two per occludee)
		SITE_OCCL_BUCKET,		// flushOccludedObjectsIntoStencil: one per non-empty player bucket
		SITE_OCCL_OTHERS,		// flushOccludedObjectsIntoStencil: neither occluder nor occludee
		SITE_OCCL_OCCLUDERS,	// flushOccludedObjectsIntoStencil: occluders
		SITE_OCCL_ALL,			// flushOccludedObjectsIntoStencil: no occluder/occludee pair - one flush
		SITE_TRANSLUCENT,		// flushTranslucentObjects
		SITE_PROJ_SHADOW,		// W3DProjectedShadowManager::renderShadows' flush: with shadow decals on, every non-occluded
								// object (main thread, before the ring's bracket)
		SITE_COUNT
	};

	int Mode();						// GX_RENDER_HYBRID, read once
	// Main thread: the next value of the ONE retire sequence every RecordExternal caller signals into the extension's shared
	// completion object (packets here, GxDepthShot's readback) - two private counters let one caller's earlier value satisfy the
	// other's wait
	uint64_t Take_Retire_Value();
	inline bool Enabled() { return Mode() != 0; }

	// Once, from RTS3DScene's constructor beside WS-G's GxScene_Install. With GX_RENDER_HYBRID set it installs
	// DX8MeshRendererClass::GxBoundary: inside every mesh-renderer flush WS-G tags as an L2 producer site (dx8renderer.h
	// GxFlushSite, mapped to Site above) it inserts after the rigid lists and after the delayed passes when that segment drew
	// something (native rigid work keeps its place ahead of classic skins and decals - DESIGN_hybrid.md §4). Unset: nothing.
	void Install();
}
