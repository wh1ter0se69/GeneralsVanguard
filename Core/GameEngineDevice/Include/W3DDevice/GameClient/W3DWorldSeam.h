// GeneralsX: THE WORLD/HUD SEAM.
//
// W3DView::draw brackets its world pass with push/pop. While GXUpscaler is off (the default) both are one branch and the frame
// issues exactly today's device calls. Otherwise push redirects the world into a private target at GXRenderScale of the current
// target, and pop resolves it, restores the previous target (the back buffer, or the pillarbox surface) and upscales the world
// into it, so everything drawn afterwards (health bars, text, InGameUI, the cursor) lands at full resolution on top. A script
// view filter frame, a headless peer and any allocation failure fall back to today's path. Render-only: the simulation reads
// none of it.
#pragma once

#include "Lib/BaseType.h"

class CameraClass;
struct IGxWorldPass;

namespace W3DWorldSeam
{
	/// Before the world pass. viewFilterActive = a script view filter runs this frame (it owns screen-sized quads: bypass).
	void push(CameraClass *camera, Bool viewFilterActive);
	/// After the world pass (the extra-render pass included), before any HUD draw.
	void pop();
	/// Drops every D3DPOOL_DEFAULT target and the device interface (DX8Wrapper::Default_Pool_Release_Hook, before Reset/Release).
	void release();
	/// Inside a redirected world pass: render height / output height (1 exactly at 100%); 1 everywhere else. For the few
	/// world-pass rules written in target pixels that must keep their on-screen size (W3DSnow's point-size clamps).
	Real activeScale();
	/// The native device's IGxWorldPass while a world pass is redirected; null otherwise (classic, seam off, outside the pass).
	/// Not AddRef'd: valid until pop().
	IGxWorldPass *worldPass();
	/// GeneralsX: F-depth+ (GxSceneTap.h): RTS3DScene::Flush calls this once the opaque world
	/// is drawn (terrain, objects, trees, shadows) and before water and every transparent. Inside the tactical view's push/pop only,
	/// once per frame, never on a view-filter frame; a no-op unless the native device has a reader on (classic: always a no-op).
	void opaqueDone();
}
