// GeneralsX: GxMotion.h (DESIGN_motion.md rev 2: §4 depth, §5 jitter, §6.9 resolve).
#include "GxMotion.h"

#include "shaders/gx_present.vert.h"
#include "shaders/gx_motion_resolve.frag.h"
#include "shaders/gx_motion_debug.frag.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace GXRender { namespace Dev {

namespace {

static_assert(sizeof(GxMotionPush) == 112, "push range");

struct DebugPush
{
	float p[4];
};

const Res::FormatInfo* rg16f()
{
	static Res::FormatInfo f = [] {
		Res::FormatInfo i;
		i.vk = VK_FORMAT_R16G16_SFLOAT;
		i.blockBytes = 4;
		i.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
		i.name = "RG16F(mv)";
		return i;
	}();
	return &f;
}

void identity(float* m)
{
	std::memset(m, 0, 64);
	m[0] = m[5] = m[10] = m[15] = 1.f;
}

// M' = M + wcol(M) * (jx, jy): clip.xy += (jx, jy) * clip.w for v * M' (row-vector D3D convention)
void jitterProjection(const float* m, float jx, float jy, float* out)
{
	std::memcpy(out, m, 64);
	for (int r = 0; r < 4; ++r) {
		out[r * 4 + 0] += m[r * 4 + 3] * jx;
		out[r * 4 + 1] += m[r * 4 + 3] * jy;
	}
}

float envFloat(const char* name, float def)
{
	const char* v = std::getenv(name);
	return v && *v ? float(std::atof(v)) : def;
}

} // namespace

void gxMat4Mul(const float* a, const float* b, float* out)
{
	float t[16];
	for (int r = 0; r < 4; ++r)
		for (int c = 0; c < 4; ++c)
			t[r * 4 + c] = a[r * 4 + 0] * b[0 * 4 + c] + a[r * 4 + 1] * b[1 * 4 + c] + a[r * 4 + 2] * b[2 * 4 + c] + a[r * 4 + 3] * b[3 * 4 + c];
	std::memcpy(out, t, 64);
}

bool gxMat4Inverse(const float* m, float* out)
{
	// Gauss-Jordan in double with partial pivoting
	double a[4][8];
	for (int r = 0; r < 4; ++r)
		for (int c = 0; c < 8; ++c)
			a[r][c] = c < 4 ? double(m[r * 4 + c]) : (c - 4 == r ? 1.0 : 0.0);
	for (int c = 0; c < 4; ++c) {
		int p = c;
		for (int r = c + 1; r < 4; ++r)
			if (std::fabs(a[r][c]) > std::fabs(a[p][c]))
				p = r;
		if (std::fabs(a[p][c]) < 1e-12)
			return false;
		if (p != c)
			for (int k = 0; k < 8; ++k)
				std::swap(a[p][k], a[c][k]);
		const double inv = 1.0 / a[c][c];
		for (int k = 0; k < 8; ++k)
			a[c][k] *= inv;
		for (int r = 0; r < 4; ++r)
			if (r != c && a[r][c] != 0.0) {
				const double f = a[r][c];
				for (int k = 0; k < 8; ++k)
					a[r][k] -= f * a[c][k];
			}
	}
	for (int r = 0; r < 4; ++r)
		for (int c = 0; c < 4; ++c)
			out[r * 4 + c] = float(a[r][c + 4]);
	return true;
}

void gxMotionHalton(uint32_t index, float* jx, float* jy)
{
	auto halton = [](uint32_t i, uint32_t b) {
		float f = 1.f, r = 0.f;
		while (i > 0) {
			f /= float(b);
			r += f * float(i % b);
			i /= b;
		}
		return r;
	};
	*jx = halton(index, 2) - 0.5f;
	*jy = halton(index, 3) - 0.5f;
}

bool GxMotion::init(Vk::VkCtx& ctx, Res::GxResources& res, GxMotionDebugView debugView, std::string* why)
{
	m_ctx = &ctx;
	m_res = &res;
	m_debugView = debugView;
	const VkDevice dev = ctx.device();
	auto module = [&](const uint32_t* code, size_t bytes) {
		VkShaderModuleCreateInfo ci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
		ci.codeSize = bytes;
		ci.pCode = code;
		VkShaderModule m = VK_NULL_HANDLE;
		vkCreateShaderModule(dev, &ci, nullptr, &m);
		return m;
	};
	m_vs = module(spv_gx_present_vert, sizeof(spv_gx_present_vert));
	m_fsResolve = module(spv_gx_motion_resolve_frag, sizeof(spv_gx_motion_resolve_frag));
	m_fsDebug = module(spv_gx_motion_debug_frag, sizeof(spv_gx_motion_debug_frag));
	VkSamplerCreateInfo sc = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };   // texelFetch only
	sc.magFilter = sc.minFilter = VK_FILTER_NEAREST;
	sc.addressModeU = sc.addressModeV = sc.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	VkDescriptorSetLayoutBinding b[2] = {};
	for (uint32_t i = 0; i < 2; ++i) {   // 0 = depth (resolve) / the debug source; 1 = the object vectors (resolve)
		b[i].binding = i;
		b[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		b[i].descriptorCount = 1;
		b[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	}
	VkDescriptorSetLayoutCreateInfo lc = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
	lc.bindingCount = 2;
	lc.pBindings = b;
	VkPushConstantRange pr = { VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(GxMotionPush) };
	VkPipelineLayoutCreateInfo pl = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
	pl.setLayoutCount = 1;
	pl.pSetLayouts = &m_setLayout;
	pl.pushConstantRangeCount = 1;
	pl.pPushConstantRanges = &pr;
	if (!m_vs || !m_fsResolve || !m_fsDebug || vkCreateSampler(dev, &sc, nullptr, &m_sampler) != VK_SUCCESS ||
		vkCreateDescriptorSetLayout(dev, &lc, nullptr, &m_setLayout) != VK_SUCCESS ||
		vkCreatePipelineLayout(dev, &pl, nullptr, &m_layout) != VK_SUCCESS) {
		*why = "motion: shader modules / sampler / layouts";
		return false;
	}
	if (!pipeline(VK_FORMAT_R16G16_SFLOAT, -1)) {
		*why = "motion: resolve pipeline";
		return false;
	}
	return true;
}

void GxMotion::shutdown()
{
	if (!m_ctx)
		return;
	const VkDevice dev = m_ctx->device();
	for (auto& kv : m_pipelines)
		vkDestroyPipeline(dev, kv.second, nullptr);
	m_pipelines.clear();
	for (Pool& p : m_pools)
		vkDestroyDescriptorPool(dev, p.pool, nullptr);
	m_pools.clear();
	if (m_mvView)
		vkDestroyImageView(dev, m_mvView, nullptr);
	m_mvView = VK_NULL_HANDLE;
	if (m_mv.image)
		m_mv.destroy(*m_res);
	if (m_layout)
		vkDestroyPipelineLayout(dev, m_layout, nullptr);
	if (m_setLayout)
		vkDestroyDescriptorSetLayout(dev, m_setLayout, nullptr);
	if (m_sampler)
		vkDestroySampler(dev, m_sampler, nullptr);
	for (VkShaderModule m : { m_vs, m_fsResolve, m_fsDebug })
		if (m)
			vkDestroyShaderModule(dev, m, nullptr);
	m_layout = VK_NULL_HANDLE;
	m_setLayout = VK_NULL_HANDLE;
	m_sampler = VK_NULL_HANDLE;
	m_vs = m_fsResolve = m_fsDebug = VK_NULL_HANDLE;
	m_ctx = nullptr;
	m_res = nullptr;
}

const float* GxMotion::begin(const GxMotionBegin& b, Res::GxImage* depthImage, VkImageView depthView, const char** whyOff)
{
	m_inWorld = true;
	m_active = false;
	m_jitterOn = false;
	m_published = false;
	m_object = nullptr;    // a registration never outlives its pass
	m_objectView = VK_NULL_HANDLE;   // nothing of this pass is visible until its resolve is recorded
	if (!b.temporal) {     // a spatial / bilinear pass: no motion, and history restarts when temporal resumes (#3)
		m_havePrev = false;
		if (whyOff)
			*whyOff = "not a temporal world pass";
		return nullptr;
	}
	if (!depthView || !depthImage || b.renderW == 0 || b.renderH == 0) {
		if (whyOff)
			*whyOff = !depthView ? "the world depth is not sampleable (no NextTargetsAreWorld(GXWP_TEMPORAL) before its creation)" : "empty world target";
		return nullptr;
	}
	if (!ensureMv(b.renderW, b.renderH)) {
		if (whyOff)
			*whyOff = "the motion-vector image could not be created";
		return nullptr;
	}
	m_active = true;
	m_depthImage = depthImage;
	m_depthView = depthView;
	GxMotionBegin bv = b;   // the camera viewport, clamped to the target (whole target when not given)
	if (bv.viewW == 0 || bv.viewH == 0 || bv.viewX >= bv.renderW || bv.viewY >= bv.renderH) {
		bv.viewX = bv.viewY = 0;
		bv.viewW = bv.renderW;
		bv.viewH = bv.renderH;
	}
	bv.viewW = std::min(bv.viewW, bv.renderW - bv.viewX);
	bv.viewH = std::min(bv.viewH, bv.renderH - bv.viewY);
	const bool resized = m_begin.renderW != bv.renderW || m_begin.renderH != bv.renderH || m_begin.viewX != bv.viewX ||
		m_begin.viewY != bv.viewY || m_begin.viewW != bv.viewW || m_begin.viewH != bv.viewH;
	m_begin = bv;

	// --- jitter (§5): Halton(2,3), 1-based, phase count int(8 * (out/render)^2) truncated, at least m_minPhases
	static const bool forceJitter = [] { const char* v = std::getenv("GX_MOTION_JITTER"); return v && *v == '1'; }();
	m_jitterOn = (b.temporal && m_consumer && m_debugView == GxMotionDebugView::None) || forceJitter;
	// FidelityFX 1.1.4 ffxFsr3GetJitterPhaseCount: int32_t(8 * ratio^2), truncated
	const float ratio = b.outputW > m_begin.viewW ? float(b.outputW) / float(m_begin.viewW) : 1.f;   // output / render viewport
	m_phaseCount = std::max(uint32_t(8.f * ratio * ratio), m_minPhases);

	// --- camera (§6.1): clipToPrevClip = inv(V * P) * (Vprev * Pprev), all unjittered
	float vp[16], invVp[16], prevVp[16], c2p[16];
	gxMat4Mul(b.view, b.proj, vp);
	bool reset = b.resetHistory || !m_havePrev || resized || !gxMat4Inverse(vp, invVp);
	if (!reset) {
		gxMat4Mul(m_prevView, m_prevProj, prevVp);
		gxMat4Mul(invVp, prevVp, c2p);
		// camera cut (§8): the screen centre and the four NDC corners, at the view depth halfway between the clip planes, moved
		// more than GX_MOTION_RESET_FRAC of the render width (a calibration knob, set from pictures) - corners catch a fast spin or
		// FOV jump the centre misses. No planes = only the seam's reset.
		static const float cutFrac = envFloat("GX_MOTION_RESET_FRAC", 0.25f);
		if (b.farZ > b.nearZ && b.nearZ >= 0.f) {
			const float vz = (b.proj[11] < 0.f ? -0.5f : 0.5f) * (b.nearZ + b.farZ);   // right-handed views look down -z
			float mid[4];   // the clip position of the view-axis point at vz: (0, 0, vz, 1) * P
			for (int c = 0; c < 4; ++c)
				mid[c] = vz * b.proj[8 + c] + b.proj[12 + c];
			const float aspect = float(m_begin.viewW) / float(m_begin.viewH);
			const float pts[5][2] = { { 0.f, 0.f }, { -1.f, -1.f }, { 1.f, -1.f }, { -1.f, 1.f }, { 1.f, 1.f } };
			for (const auto& q : pts) {
				const float clip[4] = { q[0] * mid[3], q[1] * mid[3], mid[2], mid[3] };   // the NDC point q at that depth
				float prev[4];
				for (int c = 0; c < 4; ++c)
					prev[c] = clip[0] * c2p[c] + clip[1] * c2p[4 + c] + clip[2] * c2p[8 + c] + clip[3] * c2p[12 + c];
				if (prev[3] <= 1e-6f || std::fabs(prev[0] / prev[3] - q[0]) * 0.5f > cutFrac ||
					std::fabs(prev[1] / prev[3] - q[1]) * 0.5f > cutFrac * aspect) {
					reset = true;
					break;
				}
			}
		}
	}
	if (reset || m_plants.identityReprojection)
		identity(c2p);
	if (reset)
		m_phase = 0;
	float jx = 0.f, jy = 0.f;
	if (m_jitterOn)
		gxMotionHalton((m_phase % m_phaseCount) + 1, &jx, &jy);
	++m_phase;
	const float sign = m_plants.flipJitterSign ? -1.f : 1.f;
	m_jitterNdc[0] = sign * 2.f * jx / float(m_begin.viewW);   // a pixel of the viewport the projection maps onto
	m_jitterNdc[1] = sign * -2.f * jy / float(m_begin.viewH);

	// --- the frame the backends read
	const uint64_t nowNs = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count());
	GxMotionFrame& f = m_frame;
	f.frameIndex += 1;
	f.flags = reset ? GXMF_RESET : 0u;
	f.renderW = b.renderW;
	f.renderH = b.renderH;
	f.outputW = b.outputW;
	f.outputH = b.outputH;
	f.frameTimeMs = m_lastBeginNs ? float(double(nowNs - m_lastBeginNs) * 1e-6) : 0.f;
	m_lastBeginNs = nowNs;
	f.jitterPx[0] = jx;
	f.jitterPx[1] = jy;
	f.orthographic = b.proj[11] == 0.f;
	std::memcpy(f.worldToView, b.view, 64);
	std::memcpy(f.viewToClip, b.proj, 64);
	if (!gxMat4Inverse(b.proj, f.clipToView))
		identity(f.clipToView);
	std::memcpy(f.clipToPrevClip, c2p, 64);
	if (!gxMat4Inverse(c2p, f.prevClipToClip))
		identity(f.prevClipToClip);
	float invV[16];
	if (gxMat4Inverse(b.view, invV)) {
		const float fwdSign = b.proj[11] < 0.f ? -1.f : 1.f;   // w = -z (right-handed view) looks down -z
		for (int i = 0; i < 3; ++i) {
			f.cameraRight[i] = invV[0 * 4 + i];
			f.cameraUp[i] = invV[1 * 4 + i];
			f.cameraFwd[i] = fwdSign * invV[2 * 4 + i];
			f.cameraPos[i] = invV[3 * 4 + i];
		}
	}
	f.nearZ = b.nearZ;
	f.farZ = b.farZ;
	f.fovYRad = b.fovYRad;
	f.aspect = float(m_begin.viewW) / float(m_begin.viewH);

	std::memcpy(m_prevView, b.view, 64);
	std::memcpy(m_prevProj, b.proj, 64);
	m_havePrev = true;

	if (!m_jitterOn || !m_haveClean)
		return nullptr;
	jitterProjection(m_cleanProj, m_jitterNdc[0], m_jitterNdc[1], m_jitteredProj);
	return m_jitteredProj;
}

const float* GxMotion::projection(const float* m)
{
	std::memcpy(m_cleanProj, m, 64);
	m_haveClean = true;
	if (!(m_inWorld && m_active && m_jitterOn))
		return m;
	jitterProjection(m_cleanProj, m_jitterNdc[0], m_jitterNdc[1], m_jitteredProj);
	return m_jitteredProj;
}

void GxMotion::setObject(Res::GxImage* image, VkImageView view)
{
	m_object = m_inWorld && m_active ? image : nullptr;   // null = cancel: this pass is camera-only again
	m_objectView = m_object ? view : VK_NULL_HANDLE;
}

const float* GxMotion::end(bool* record, GxMotionResolve* cmd, Res::GxImage* colour, VkImageView colourView)
{
	*record = m_inWorld && m_active && m_depthImage && m_mv.image && m_mvView;
	const bool jittered = m_inWorld && m_active && m_jitterOn;
	m_inWorld = false;
	m_active = false;
	Res::GxImage* object = m_object;   // this pass's registration only: not registered next pass = camera-only
	const VkImageView objectView = m_objectView;
	m_object = nullptr;
	m_objectView = VK_NULL_HANDLE;
	if (*record) {
		GxMotionResolve& r = *cmd;
		r.depth = m_depthImage;
		r.depthView = m_depthView;
		r.mv = &m_mv;
		r.mvView = m_mvView;
		r.object = object;
		r.objectView = objectView;
		r.w = m_begin.renderW;
		r.h = m_begin.renderH;
		r.view = { { int32_t(m_begin.viewX), int32_t(m_begin.viewY) }, { m_begin.viewW, m_begin.viewH } };
		// the camera viewport the translator rasterised with (GxD3D8Translate.cpp:782-785: origin + 0.5, negative height); pixels
		// outside it (a control bar strip) keep zero vectors - nothing of the world is drawn there
		const float vx = float(m_begin.viewX), vy = float(m_begin.viewY), vw = float(m_begin.viewW), vh = float(m_begin.viewH);
		GxMotionPush& p = r.push;
		std::memcpy(p.clipToPrevClip, m_frame.clipToPrevClip, 64);
		p.viewport[0] = 0.5f * vw;
		p.viewport[1] = 0.5f * vh;
		p.viewport[2] = vx + 0.5f * vw + 0.5f;
		p.viewport[3] = vy + 0.5f * vh + 0.5f;
		p.jitterDepth[0] = m_plants.jitterInVelocity ? 0.f : m_frame.jitterPx[0];
		p.jitterDepth[1] = m_plants.jitterInVelocity ? 0.f : m_frame.jitterPx[1];
		// the depth range exactly as the translator hands it to Vulkan (GxD3D8Translate.cpp:782-785): clip z = (d - z0) / (z1 - z0)
		const float z0 = std::clamp(m_begin.minZ, 0.f, 1.f);
		const float z1 = std::clamp(std::max(m_begin.maxZ, m_begin.minZ + (m_begin.minZ >= 0.5f ? 0.f : 0.001f)), 0.f, 1.f);
		p.jitterDepth[2] = z0;
		p.jitterDepth[3] = z1 > z0 ? 1.f / (z1 - z0) : 1.f;
		p.flags[0] = object && !m_plants.ignoreObject ? 1.f : 0.f;
		if (m_debugView != GxMotionDebugView::None && colour && colourView && colour->format) {
			r.colour = colour;
			r.colourView = colourView;
			r.debugMode = m_debugView == GxMotionDebugView::Depth ? 0 : 1;
			r.debug[0] = r.debugMode == 0 ? m_frame.nearZ : 16.f;   // velocity: full brightness at 16 px
			r.debug[1] = m_frame.farZ;
			r.debug[2] = p.jitterDepth[2];
			r.debug[3] = p.jitterDepth[3];
		}
		// what the backends read once it is published: the layouts record() leaves the images in
		GxMotionFrame& f = m_frame;
		f.flags = (f.flags & ~uint32_t(GXMF_HAS_OBJECT_MV)) | (p.flags[0] > 0.5f ? uint32_t(GXMF_HAS_OBJECT_MV) : 0u);
		f.depth.image = m_depthImage->image;
		f.depth.view = m_depthView;
		f.depth.format = m_depthImage->format ? m_depthImage->format->vk : VK_FORMAT_UNDEFINED;
		f.depth.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
		f.depth.width = m_depthImage->width;
		f.depth.height = m_depthImage->height;
		f.mv.image = m_mv.image;
		f.mv.view = m_mvView;
		f.mv.format = VK_FORMAT_R16G16_SFLOAT;
		f.mv.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		f.mv.width = r.w;
		f.mv.height = r.h;
	}
	m_depthImage = nullptr;
	m_depthView = VK_NULL_HANDLE;
	if (!jittered || !m_haveClean || m_plants.jitterAfterEnd)
		return nullptr;
	return m_cleanProj;
}

void GxMotion::published(bool ok)
{
	m_published = ok;
	if (!ok)
		m_havePrev = false;   // the next pass starts a new history (GXMF_RESET)
}

const float* GxMotion::abandon()
{
	if (!m_inWorld)
		return nullptr;   // no open pass: nothing to abandon, history kept (every Begin asks)
	const bool jittered = m_active && m_jitterOn;
	m_inWorld = m_active = m_published = m_havePrev = false;
	m_object = nullptr;
	m_objectView = VK_NULL_HANDLE;
	m_depthImage = nullptr;
	m_depthView = VK_NULL_HANDLE;
	return jittered && m_haveClean ? m_cleanProj : nullptr;
}

bool GxMotion::ensureMv(uint32_t w, uint32_t h)
{
	if (m_mv.image && m_mvView && m_mv.width == w && m_mv.height == h)
		return true;
	if (m_mvView) {
		const VkDevice dev = m_ctx->device();
		const VkImageView v = m_mvView;
		m_ctx->retire(Vk::QueueKind::Graphics, m_mv.lastUse, [dev, v]() { vkDestroyImageView(dev, v, nullptr); });
		m_mvView = VK_NULL_HANDLE;
	}
	if (m_mv.image)
		m_mv.destroy(*m_res);
	m_mv = Res::GxImage();
	std::string why;
	if (!m_mv.create(*m_res, rg16f(), w, h, 1, VK_SAMPLE_COUNT_1_BIT,
			VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, &why))
		return false;
	VkImageViewCreateInfo ci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
	ci.image = m_mv.image;
	ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
	ci.format = VK_FORMAT_R16G16_SFLOAT;
	ci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
	if (vkCreateImageView(m_ctx->device(), &ci, nullptr, &m_mvView) == VK_SUCCESS)
		return true;
	m_mvView = VK_NULL_HANDLE;   // no half-made image survives to pass the fast path
	m_mv.destroy(*m_res);
	m_mv = Res::GxImage();
	return false;
}

VkPipeline GxMotion::pipeline(VkFormat colour, int mode)
{
	const int64_t key = (int64_t(colour) << 8) | int64_t(mode + 1);
	auto it = m_pipelines.find(key);
	if (it != m_pipelines.end())
		return it->second;
	const int32_t debugMode = mode < 0 ? 0 : mode;
	const VkSpecializationMapEntry map = { 0, 0, sizeof(int32_t) };
	VkSpecializationInfo si = { 1, &map, sizeof(debugMode), &debugMode };
	VkPipelineShaderStageCreateInfo st[2] = { { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO },
		{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO } };
	st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	st[0].module = m_vs;
	st[0].pName = "main";
	st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	st[1].module = mode < 0 ? m_fsResolve : m_fsDebug;
	st[1].pName = "main";
	st[1].pSpecializationInfo = mode < 0 ? nullptr : &si;
	VkPipelineRenderingCreateInfo ri = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
	ri.colorAttachmentCount = 1;
	ri.pColorAttachmentFormats = &colour;
	VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
	VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
	ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
	vp.viewportCount = 1;
	vp.scissorCount = 1;
	VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
	rs.cullMode = VK_CULL_MODE_NONE;
	rs.polygonMode = VK_POLYGON_MODE_FILL;
	rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	rs.lineWidth = 1.0f;
	const VkSampleMask sampleMask = 0x1;
	VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
	ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
	ms.pSampleMask = &sampleMask;
	VkPipelineColorBlendAttachmentState cb = {};
	cb.colorWriteMask = mode < 0 ? (VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT) :
		(VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT);
	VkPipelineColorBlendStateCreateInfo cbs = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
	cbs.attachmentCount = 1;
	cbs.pAttachments = &cb;
	const VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
	VkPipelineDynamicStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
	ds.dynamicStateCount = 2;
	ds.pDynamicStates = dyn;
	VkGraphicsPipelineCreateInfo pi = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
	pi.pNext = &ri;
	pi.stageCount = 2;
	pi.pStages = st;
	pi.pVertexInputState = &vi;
	pi.pInputAssemblyState = &ia;
	pi.pViewportState = &vp;
	pi.pRasterizationState = &rs;
	pi.pMultisampleState = &ms;
	pi.pColorBlendState = &cbs;
	pi.pDynamicState = &ds;
	pi.layout = m_layout;
	VkPipeline p = VK_NULL_HANDLE;
	if (vkCreateGraphicsPipelines(m_ctx->device(), VK_NULL_HANDLE, 1, &pi, nullptr, &p) != VK_SUCCESS)
		return VK_NULL_HANDLE;
	m_pipelines[key] = p;
	return p;
}

VkDescriptorSet GxMotion::allocSet(VkImageView view, VkImageLayout layout, VkImageView view1, VkImageLayout layout1)
{
	const VkDevice dev = m_ctx->device();
	if (m_curPool < 0) {   // a pool whose last frame completed, or a new one
		const uint64_t done = m_res->completed();
		for (size_t i = 0; i < m_pools.size() && m_curPool < 0; ++i)
			if (m_pools[i].lastUse <= done) {
				vkResetDescriptorPool(dev, m_pools[i].pool, 0);
				m_curPool = int(i);
			}
		if (m_curPool < 0) {
			VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8 };
			VkDescriptorPoolCreateInfo pc = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
			pc.maxSets = 4;
			pc.poolSizeCount = 1;
			pc.pPoolSizes = &ps;
			Pool p;
			if (vkCreateDescriptorPool(dev, &pc, nullptr, &p.pool) != VK_SUCCESS)
				return VK_NULL_HANDLE;
			m_pools.push_back(p);
			m_curPool = int(m_pools.size() - 1);
		}
	}
	Pool& pool = m_pools[size_t(m_curPool)];
	pool.lastUse = m_res->nextUseValue();
	VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
	ai.descriptorPool = pool.pool;
	ai.descriptorSetCount = 1;
	ai.pSetLayouts = &m_setLayout;
	VkDescriptorSet set = VK_NULL_HANDLE;
	if (vkAllocateDescriptorSets(dev, &ai, &set) != VK_SUCCESS)
		return VK_NULL_HANDLE;
	const VkDescriptorImageInfo ii[2] = { { m_sampler, view, layout },
		{ m_sampler, view1 ? view1 : view, view1 ? layout1 : layout } };   // binding 1 always valid (the source again when unused)
	VkWriteDescriptorSet w[2] = { { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET }, { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET } };
	for (uint32_t i = 0; i < 2; ++i) {
		w[i].dstSet = set;
		w[i].dstBinding = i;
		w[i].descriptorCount = 1;
		w[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		w[i].pImageInfo = &ii[i];
	}
	vkUpdateDescriptorSets(dev, 2, w, 0, nullptr);
	return set;
}

uint32_t GxMotion::touched(const GxMotionResolve& r, Res::GxImage** out)
{
	uint32_t n = 0;
	for (Res::GxImage* i : { r.depth, r.mv, r.object, r.colour })
		if (i)
			out[n++] = i;
	return n;
}

bool GxMotion::record(VkCommandBuffer cmd, const GxMotionResolve& r)
{
	if (!r.depth || !r.mv || !r.mvView)
		return false;
	m_curPool = -1;
	const VkPipeline resolve = pipeline(VK_FORMAT_R16G16_SFLOAT, -1);
	const VkImageLayout depthLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
	const VkDescriptorSet depthSet = allocSet(r.depthView, depthLayout, r.objectView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	if (!resolve || !depthSet)
		return false;
	r.depth->transition(cmd, depthLayout, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
	if (r.object)
		r.object->transition(cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
			VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
	r.mv->transition(cmd, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
		VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
	const VkViewport viewport = { 0.f, 0.f, float(r.w), float(r.h), 0.f, 1.f };
	const VkRect2D whole = { { 0, 0 }, { r.w, r.h } };
	auto pass = [&](VkImageView target, VkAttachmentLoadOp load, VkPipeline p, VkDescriptorSet set, const void* push, uint32_t bytes,
		const VkRect2D& draw) {
		VkRenderingAttachmentInfo at = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
		at.imageView = target;
		at.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		at.loadOp = load;   // CLEAR = zero vectors where the pass does not draw
		at.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO };
		ri.renderArea = whole;
		ri.layerCount = 1;
		ri.colorAttachmentCount = 1;
		ri.pColorAttachments = &at;
		vkCmdBeginRendering(cmd, &ri);
		vkCmdSetViewport(cmd, 0, 1, &viewport);
		vkCmdSetScissor(cmd, 0, 1, &draw);
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_layout, 0, 1, &set, 0, nullptr);
		vkCmdPushConstants(cmd, m_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, bytes, push);
		vkCmdDraw(cmd, 3, 1, 0, 0);
		vkCmdEndRendering(cmd);
	};
	pass(r.mvView, VK_ATTACHMENT_LOAD_OP_CLEAR, resolve, depthSet, &r.push, sizeof(r.push), r.view);
	r.mv->transition(cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
		VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_TRANSFER_READ_BIT);
	const uint64_t use = m_res->nextUseValue();
	r.mv->markUsed(use);
	r.depth->markUsed(use);
	if (r.object)
		r.object->markUsed(use);

	if (r.debugMode >= 0 && r.colour) {
		const VkPipeline dbg = pipeline(r.colour->format->vk, r.debugMode);
		const VkDescriptorSet set = r.debugMode == 0 ? allocSet(r.depthView, depthLayout, VK_NULL_HANDLE, depthLayout) :
			allocSet(r.mvView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		if (dbg && set) {
			const DebugPush dp = { { r.debug[0], r.debug[1], r.debug[2], r.debug[3] } };
			// LOAD reads the world's colour: the barrier makes those writes visible to the attachment read
			r.colour->transition(cmd, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
				VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
			pass(r.colourView, VK_ATTACHMENT_LOAD_OP_LOAD, dbg, set, &dp, sizeof(dp), whole);
			r.colour->markUsed(use);
		}
	}
	return true;
}

}} // namespace GXRender::Dev
