// GeneralsX: What the temporal upscalers read from a world pass: depth, motion vectors,
// jitter, camera (modernize/render-dlss/DESIGN_motion.md §2). ONE internal convention; each backend (DLSS/Streamline, FSR 3.1,
// MetalFX, XeSS) maps it with scale/flag parameters only:
//   motion vectors  RG16_SFLOAT, render size, CURRENT -> PREVIOUS (prev = cur + mv) in render PIXELS, +x right / +y down,
//                   camera motion included, jitter EXCLUDED, not dilated   (DLSS InMVScale 1,1 / Streamline mvecScale 1/renderW,1/renderH;
//                   FSR motionVectorScale 1,1; MetalFX motionVectorScale 1,1; XeSS velocity scale 1,1)
//   depth           the world pass's own depth-stencil, single sample, 0 = near / 1 = far (NOT reversed, not infinite);
//                   read through depthView (DEPTH aspect only)
//   jitter          pixels in [-0.5,0.5], +x right / +y down; the device offsets clip.xy by (+2jx/viewW, -2jy/viewH) * clip.w, view = the camera viewport inside
//                   the world target (the whole target when the camera covers it)
//   matrices        D3D row-vector convention (clip = pos * M), stored row-major like D3DMATRIX, UNJITTERED
// Valid from IGxWorldPass::End until the next Begin; filled on the device's front side, carried by value into the backend's
// recorded command (A23-B: owned arguments). The device tracks each image's layout: a backend that transitions depth or mv must
// leave them in the layout given here (or go through the device's GxImage). Render-only: nothing here is read by the simulation.
#pragma once
#include "VkCtx.h"
#include <stdint.h>

namespace GXRender { namespace Dev {

struct GxMotionImage
{
	VkImage image = VK_NULL_HANDLE;
	VkImageView view = VK_NULL_HANDLE;      // single-aspect view the backend samples
	VkFormat format = VK_FORMAT_UNDEFINED;
	VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;   // the layout the image is in when the backend's command runs
	uint32_t width = 0, height = 0;
};

enum GxMotionFrameFlags : uint32_t
{
	GXMF_RESET          = 1u << 0,   // history invalid: seam GXWP_RESET_HISTORY, first frame, or a camera cut the device detected
	GXMF_HAS_OBJECT_MV  = 1u << 1,   // render-motion-units-1's per-object vectors are merged into mv (else camera-only)
	GXMF_HAS_REACTIVE   = 1u << 2,   // reactive is valid
};

struct GxMotionFrame
{
	uint64_t frameIndex = 0;            // +1 per ACTIVE temporal world pass (FSR FG wants exactly +1); spatial/inactive passes skip it
	uint32_t flags = 0;                 // GxMotionFrameFlags
	uint32_t renderW = 0, renderH = 0;  // world target size
	uint32_t outputW = 0, outputH = 0;  // upscale destination size
	float frameTimeMs = 0.f;            // since the previous active temporal world pass (FSR ms; MetalFX wants seconds)

	GxMotionImage depth;                // DEPTH aspect view; D24_UNORM_S8_UINT where the GPU supports it, else D32_SFLOAT_S8_UINT (GxFormat.cpp:90-93; Apple GPUs take the fallback)
	GxMotionImage mv;                   // RG16_SFLOAT, convention above
	GxMotionImage reactive;             // R8_UNORM, 0 = normal .. 1 = ignore history; image == VK_NULL_HANDLE until units-1 lands it

	float jitterPx[2] = { 0.f, 0.f };   // this frame's jitter in render pixels (0,0 when no temporal backend consumes it)
	float mvScale[2] = { 1.f, 1.f };    // multiply mv by this to get render pixels (always 1,1 here)
	bool depthInverted = false;         // never: 0 near, 1 far
	bool mvJittered = false;            // never: jitter is excluded
	bool mvDilated = false;             // never
	bool orthographic = false;

	// camera, unjittered, D3D row-vector convention (clip = viewPos * viewToClip)
	float worldToView[16] = {};
	float viewToClip[16] = {};
	float clipToView[16] = {};
	float clipToPrevClip[16] = {};      // this frame's clip -> previous frame's clip (Streamline clipToPrevClip); identity on GXMF_RESET
	float prevClipToClip[16] = {};
	float cameraPos[3] = {}, cameraRight[3] = {}, cameraUp[3] = {}, cameraFwd[3] = {};   // world space, orthonormal (fwd = view direction)
	float nearZ = 0.f, farZ = 0.f;
	float fovYRad = 0.f;                // vertical field of view, radians (MetalFX wants degrees)
	float aspect = 0.f;                 // viewW / viewH of the camera viewport (= renderW / renderH when it covers the target)
};

} }
