// GeneralsX: depth, camera motion vectors and jitter for the temporal upscalers
// (modernize/render-dlss/DESIGN_motion.md rev 2).
//
// Between IGxWorldPass::Begin and End (the world pass of a frame) the device:
//  * jitters the projection on the FRONT side: SetTransform(PROJECTION, M) hands the translator M + wcol(M) * jitterNdc, so
//    clip.xy moves by jitterNdc * clip.w; GetTransform answers the clean copy (it is kept from the first motion frame on);
//  * at End records ONE velocity resolve pass (gx_motion_resolve.frag) reading the world depth and writing the mv image:
//    camera motion for every pixel, current -> previous in render pixels (GxMotionFrame.h), and optionally a GX_MOTION_DEBUG view
//    over the world colour - both inside the same outside-scope region as the seam's End work;
//  * publishes the frame's GxMotionFrame for the upscaler backends.
// Off (no Begin seen, or the depth not sampleable) = no transform touched, no pass, nothing allocated. Render-only.
#pragma once

#include "GxMotionFrame.h"
#include "GxResources.h"

#include <map>
#include <string>
#include <vector>

namespace GXRender { namespace Dev {

// What Begin carries (the device converts IGxWorldPass's GxWorldPassBegin; tests fill it directly).
struct GxMotionBegin
{
	uint32_t renderW = 0, renderH = 0, outputW = 0, outputH = 0;
	// the viewport the 3D draws use inside the world target (CameraClass::Apply's rect); viewW == 0 = the whole target
	uint32_t viewX = 0, viewY = 0, viewW = 0, viewH = 0;
	float minZ = 0.f, maxZ = 1.f;   // that viewport's depth range (CameraClass ZBufferMin/Max)
	bool resetHistory = false;     // GXWP_RESET_HISTORY
	bool temporal = false;         // GXWP_TEMPORAL: a temporal upscaler consumes the jitter
	float view[16];                // D3D row-vector, unjittered: what CameraClass::Apply sets
	float proj[16];
	float nearZ = 0.f, farZ = 0.f, fovYRad = 0.f;
};

enum class GxMotionDebugView { None, Depth, Velocity };

// The resolve's push block (gx_motion_resolve.frag), 112 bytes.
struct GxMotionPush
{
	float clipToPrevClip[16];
	float viewport[4];      // W/2, H/2, centre x, centre y of the camera viewport (framebuffer pixels, D3D9 0.5 offset included)
	float jitterDepth[4];   // jitter px x, y; minDepth; 1 / (maxDepth - minDepth)
	float flags[4];         // x = 1: binding 1 holds this pass's object vectors
};

// End's resolve as ONE command with owned arguments (A23-B): everything it reads, captured by value on
// the front side at End. record() reads only this and its own pipelines/pools.
struct GxMotionResolve
{
	Res::GxImage* depth = nullptr;
	VkImageView depthView = VK_NULL_HANDLE;
	Res::GxImage* mv = nullptr;
	VkImageView mvView = VK_NULL_HANDLE;
	Res::GxImage* object = nullptr;        // render-motion-units-1's vectors, or null
	VkImageView objectView = VK_NULL_HANDLE;
	Res::GxImage* colour = nullptr;        // the world colour, for a GX_MOTION_DEBUG view (null = none)
	VkImageView colourView = VK_NULL_HANDLE;
	uint32_t w = 0, h = 0;
	VkRect2D view = {};
	GxMotionPush push = {};
	int debugMode = -1;                    // -1 none, 0 depth, 1 velocity
	float debug[4] = {};
};

// TEST-ONLY plants (gx_native_test X- arms); all false in the product.
struct GxMotionPlants
{
	bool identityReprojection = false;   // clipToPrevClip = identity: camera motion vanishes (mv-camera must fail)
	bool jitterInVelocity = false;       // the resolve does not take the jitter out (mv-zoom-jitter must fail)
	bool flipJitterSign = false;         // the projection offset has the wrong sign (jitter-sign must fail)
	bool jitterAfterEnd = false;         // End does not re-send the clean projection (jitter-scope must fail)
	bool ignoreObject = false;           // the resolve never reads the registered object vectors (mv-object must fail)
};

class GxMotion
{
public:
	bool init(Vk::VkCtx& ctx, Res::GxResources& res, GxMotionDebugView debugView, std::string* why);
	void shutdown();

	// ---- front side (the thread driving the device)
	bool inWorld() const { return m_inWorld; }
	// The projection jitter in force (NDC), (0,0) outside a jittering world pass: IGxWorldPass::GetJitter, for custom VS whose
	// world-view-projection the game builds itself.
	void jitterNdc(float ndc[2]) const
	{
		const bool on = m_inWorld && m_active && m_jitterOn;
		ndc[0] = on ? m_jitterNdc[0] : 0.f;
		ndc[1] = on ? m_jitterNdc[1] : 0.f;
	}
	// Begin of a world pass. depthView/depthImage: the world depth's DEPTH-aspect view (null = not sampleable: motion is off for
	// this pass and whyOff says why). Returns the projection to re-send to the translator (jittered), or null when nothing changes.
	const float* begin(const GxMotionBegin& b, Res::GxImage* depthImage, VkImageView depthView, const char** whyOff);
	// SetTransform(PROJECTION) hook: keeps the clean copy; returns what the translator gets (the jittered matrix inside a world
	// pass that jitters, else m itself).
	const float* projection(const float* m);
	// GetTransform(PROJECTION): the clean copy, or null when the motion state never saw one (the caller answers as before).
	const float* cleanProjection() const { return m_haveClean ? m_cleanProj : nullptr; }
	// IGxWorldPass::SetObjectVelocity: render-motion-units-1's object vectors for THIS world pass (RGBA16F, a > 0.5 = written,
	// last written as a colour attachment); read by End's resolve, then forgotten. Outside an active pass: ignored.
	void setObject(Res::GxImage* image, VkImageView view);
	// End of the world pass: the clean projection to re-send (null = none); *record = *cmd must be recorded now (cmd filled with
	// this pass's inputs; colour = the world colour for a debug view).
	const float* end(bool* record, GxMotionResolve* cmd, Res::GxImage* colour, VkImageView colourView);
	// After the recording: publish this pass's frame (ok), or drop it and restart history (failure).
	void published(bool ok);
	// An open world pass is abandoned (a Reset, or a Begin without its End): nothing recorded, history restarts. Returns the clean
	// projection to re-send when that pass had installed a jittered one, else null.
	const float* abandon();

	// ---- recording (inside the writer's outsideScope): the resolve, then the debug view over colour
	bool record(VkCommandBuffer cmd, const GxMotionResolve& r);
	// Images r touches (for the writer's tracker). out must hold 4.
	static uint32_t touched(const GxMotionResolve& r, Res::GxImage** out);

	const GxMotionFrame& frame() const { return m_frame; }
	bool isPublished() const { return m_published; }   // the last world pass's frame is valid (motionFrame)
	Res::GxImage& mvImage() { return m_mv; }   // TESTS (read-back)
	GxMotionPlants& plants() { return m_plants; }
	uint32_t jitterPhaseCount() const { return m_phaseCount; }
	void setMinJitterPhases(uint32_t n) { m_minPhases = n; }   // DLSS asks for >= 16
	// A temporal backend that WILL consume this session's jitter says so: without one, a GXWP_TEMPORAL
	// pass is not jittered (a bilinear fallback must not show a jittered world). GX_MOTION_JITTER=1 forces it (tests).
	void setJitterConsumer(bool on) { m_consumer = on; }

private:
	bool ensureMv(uint32_t w, uint32_t h);
	VkPipeline pipeline(VkFormat colour, int mode);   // mode -1 = resolve, 0/1 = debug depth/velocity
	VkDescriptorSet allocSet(VkImageView view, VkImageLayout layout, VkImageView view1, VkImageLayout layout1);

	Vk::VkCtx* m_ctx = nullptr;
	Res::GxResources* m_res = nullptr;
	GxMotionDebugView m_debugView = GxMotionDebugView::None;
	GxMotionPlants m_plants;

	VkShaderModule m_vs = VK_NULL_HANDLE, m_fsResolve = VK_NULL_HANDLE, m_fsDebug = VK_NULL_HANDLE;
	VkSampler m_sampler = VK_NULL_HANDLE;
	VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;
	VkPipelineLayout m_layout = VK_NULL_HANDLE;
	std::map<int64_t, VkPipeline> m_pipelines;
	struct Pool { VkDescriptorPool pool = VK_NULL_HANDLE; uint64_t lastUse = 0; };
	std::vector<Pool> m_pools;   // one per recorded frame, reset once its submission completed
	int m_curPool = -1;

	Res::GxImage m_mv;
	VkImageView m_mvView = VK_NULL_HANDLE;

	// front state
	bool m_inWorld = false, m_active = false, m_haveClean = false, m_havePrev = false, m_jitterOn = false, m_consumer = false;
	bool m_published = false;
	float m_cleanProj[16], m_jitteredProj[16];
	float m_prevView[16], m_prevProj[16];
	float m_jitterNdc[2] = { 0.f, 0.f };
	uint32_t m_phaseCount = 8, m_minPhases = 8, m_phase = 0;
	uint64_t m_lastBeginNs = 0;
	Res::GxImage* m_depthImage = nullptr;
	Res::GxImage* m_object = nullptr;
	VkImageView m_objectView = VK_NULL_HANDLE;
	VkImageView m_depthView = VK_NULL_HANDLE;
	GxMotionBegin m_begin;
	GxMotionFrame m_frame;
};

// Matrix helpers (row-major float[16], D3D row-vector convention: (v * a) * b = v * mul(a, b)). Exposed for the tests.
void gxMat4Mul(const float* a, const float* b, float* out);
bool gxMat4Inverse(const float* m, float* out);
// The jitter in render pixels for a 1-based Halton(2,3) index: each component in [-0.5, 0.5].
void gxMotionHalton(uint32_t index, float* jx, float* jy);

}} // namespace GXRender::Dev
