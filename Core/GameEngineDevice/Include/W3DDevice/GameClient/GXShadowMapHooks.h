// GeneralsX: S7 GPU sun shadow map: the Core side of the game glue
// (modernize/look-director/d2/PREREG_look-shadowmaps.md; PLAN Task 5).
//
// W3DView.cpp (Core: both games) defines these and calls the frame hooks around the tactical scene; the ZH glue
// (GeneralsMD/.../Shadow/W3DShadowMap.cpp) installs them when the native device answers IGxShadowMap (GXShadowMap on). Null / false
// (the default: classic, key off, Generals) = no call, today's shadows. Render-only: nothing here is read by the simulation.
#pragma once

class CameraClass;
class SceneClass;

struct GxShadowMapFrameHooks
{
	void (*tacticalBegin)(CameraClass *camera);	// before the tactical scene's first world draw (the walk, the fit, BeginShadowFrame)
	void (*tacticalEnd)();						// after it (every tag cleared)
};
extern GxShadowMapFrameHooks *GxShadowMapFrame;
// A49 (D · look-shadowmaps-4): W3DView::draw ARMS the tactical frame with its camera; RTS3DScene::Customized_Render (ZH) calls
// GxShadowMapFireArmed(&rinfo.Camera) right after its On_Frame_Update loop - the frame starts there (every pose it causes then lands where
// key off's DoShadows poses do: after On_Frame_Update), before the first object is submitted. Null = nothing armed (key off, classic, Generals).
extern CameraClass *GxShadowMapArmedCamera;
extern SceneClass *GxShadowMapArmedScene;	// A51: the tactical scene W3DView armed (a nested render of another scene never fires it)
extern SceneClass *GxShadowMapFrameScene;	// A51: the scene that fired this frame's start (the glue reads its objects' visible bits)
extern bool GxShadowMapXNoFire;	// TEST-ONLY GX_SHADOWMAP_X_NOFIRE=1 (A50 (4)'s X-): armed, never fired - tacticalEnd must count it
inline void GxShadowMapFireArmed(CameraClass *camera, SceneClass *scene)
{
	if (GxShadowMapArmedCamera != nullptr && GxShadowMapArmedCamera == camera && GxShadowMapArmedScene == scene &&
		GxShadowMapFrame != nullptr && !GxShadowMapXNoFire)
	{
		GxShadowMapArmedCamera = nullptr;
		GxShadowMapFrameScene = scene;
		GxShadowMapFrame->tacticalBegin(camera);
	}
}
// This frame's map is ON (step < 3): the ZH volumetric manager skips today's volumes and their darkening quad.
extern bool GxShadowMapActive;
// Set by W3DView::update when the terrain rebuilds (a lowered cell's partial update or a full update): the fit re-scans the terrain's
// height range before its next frame and clears it (a crater must not wait for a periodic scan).
extern bool GxShadowMapTerrainChanged;
