// GeneralsX: F-depth+ (see GxSceneCopy.h).
#include "GxSceneCopy.h"

#include "GxFormat.h"
#include "shaders/gx_present.vert.h"
#include "shaders/gx_fdepth_probe.frag.h"

#include <cstdio>
#include <cstring>

namespace GXRender { namespace Dev {

namespace R = GXRender::Res;
namespace Rc = GXRender::Rec;

namespace {
const R::FormatInfo* rgba32f()   // the probe's 1x1 target: no D3D8 format maps to it
{
	static R::FormatInfo f = [] {
		R::FormatInfo i;
		i.vk = VK_FORMAT_R32G32B32A32_SFLOAT;
		i.blockBytes = 16;
		i.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
		i.name = "RGBA32F(fdepth probe)";
		return i;
	}();
	return &f;
}
} // namespace

bool GxSceneReaders::set(const char* name, uint32_t needs)
{
	needs &= GX_SCENE_DEPTH | GX_SCENE_COLOUR;
	const std::string n = name ? name : "";
	bool found = false;
	for (Entry& e : m_entries)
		if (e.name == n) {
			e.needs = needs;
			found = true;
		}
	if (!found)
		m_entries.push_back({ n, needs });
	uint32_t u = 0;
	for (const Entry& e : m_entries)
		u |= e.needs;
	const bool changed = u != m_needs;
	m_needs = u;
	return changed;
}

std::string GxSceneReaders::describe() const
{
	std::string s;
	for (const Entry& e : m_entries) {
		if (!s.empty())
			s += ' ';
		s += e.name + "=" + ((e.needs & GX_SCENE_DEPTH) ? "d" : "") + ((e.needs & GX_SCENE_COLOUR) ? "c" : "") + (e.needs ? "" : "off");
	}
	return s;
}

bool GxSceneCopy::init(Vk::VkCtx& ctx, R::GxResources& res, uint32_t probeNeeds, bool forcePass, std::string* why)
{
	m_ctx = &ctx;
	m_res = &res;
	m_forcePass = forcePass;
	m_probeNeeds = probeNeeds & (GX_SCENE_DEPTH | GX_SCENE_COLOUR);
	if (!m_probeNeeds)
		return true;
	// the dummy reader: everything it records is built now - a reader never compiles a pipeline at a toggle (S14)
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
	m_fs = module(spv_gx_fdepth_probe_frag, sizeof(spv_gx_fdepth_probe_frag));
	VkSamplerCreateInfo sc = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };   // texelFetch only
	sc.magFilter = sc.minFilter = VK_FILTER_NEAREST;
	sc.addressModeU = sc.addressModeV = sc.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	VkDescriptorSetLayoutBinding b[2] = {};
	for (uint32_t i = 0; i < 2; ++i) {
		b[i].binding = i;
		b[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		b[i].descriptorCount = 1;
		b[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	}
	VkDescriptorSetLayoutCreateInfo lc = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
	lc.bindingCount = 2;
	lc.pBindings = b;
	VkPushConstantRange pr = { VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16 };
	VkPipelineLayoutCreateInfo pl = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
	pl.setLayoutCount = 1;
	pl.pSetLayouts = &m_setLayout;
	pl.pushConstantRangeCount = 1;
	pl.pPushConstantRanges = &pr;
	if (!m_vs || !m_fs || vkCreateSampler(dev, &sc, nullptr, &m_sampler) != VK_SUCCESS ||
		vkCreateDescriptorSetLayout(dev, &lc, nullptr, &m_setLayout) != VK_SUCCESS ||
		vkCreatePipelineLayout(dev, &pl, nullptr, &m_layout) != VK_SUCCESS) {
		*why = "fdepth probe: shader modules / sampler / layouts";
		return false;
	}
	const VkFormat outFmt = VK_FORMAT_R32G32B32A32_SFLOAT;
	VkPipelineShaderStageCreateInfo st[2] = { { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO },
		{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO } };
	st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	st[0].module = m_vs;
	st[0].pName = "main";
	st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	st[1].module = m_fs;
	st[1].pName = "main";
	VkPipelineRenderingCreateInfo ri = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
	ri.colorAttachmentCount = 1;
	ri.pColorAttachmentFormats = &outFmt;
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
	cb.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
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
	if (vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &pi, nullptr, &m_pipeline) != VK_SUCCESS) {
		m_pipeline = VK_NULL_HANDLE;
		*why = "fdepth probe: pipeline";
		return false;
	}
	if (!m_probeImg.create(res, rgba32f(), 1, 1, 1, VK_SAMPLE_COUNT_1_BIT,
			VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, why))
		return false;
	VkImageViewCreateInfo vc = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
	vc.image = m_probeImg.image;
	vc.viewType = VK_IMAGE_VIEW_TYPE_2D;
	vc.format = outFmt;
	vc.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
	if (vkCreateImageView(dev, &vc, nullptr, &m_probeView) != VK_SUCCESS) {
		m_probeView = VK_NULL_HANDLE;
		*why = "fdepth probe: view";
		return false;
	}
	m_readers.set("probe", m_probeNeeds);
	return true;
}

void GxSceneCopy::retire(Rc::GxPacketWriter& w, R::GxRenderTarget* rt)
{
	if (!rt)
		return;
	w.forget(&rt->image());   // the writer drops its tracker state and any GxScopeResolve that names it
	rt->destroy();            // views and image retired at the image's last use
	delete rt;
}

void GxSceneCopy::dropTargets(Rc::GxPacketWriter& w)
{
	disarm(w);
	retire(w, m_depthRt.release());
	retire(w, m_colourRt.release());
	const uint64_t serial = m_frame.serial;
	m_frame = GxSceneFrame();
	m_frame.serial = serial;
	m_failedFmt[0] = m_failedFmt[1] = 0;
	m_fmt[0] = m_fmt[1] = 0;
}

void GxSceneCopy::shutdown(Rc::GxPacketWriter& w)
{
	dropTargets(w);
	if (!m_ctx)
		return;
	const VkDevice dev = m_ctx->device();
	if (m_probeImg.image) {
		w.forget(&m_probeImg);
		if (m_probeView) {
			const VkImageView v = m_probeView;
			m_ctx->retire(Vk::QueueKind::Graphics, m_probeImg.lastUse, [dev, v]() { vkDestroyImageView(dev, v, nullptr); });
		}
		m_probeImg.destroy(*m_res);
	}
	m_probeView = VK_NULL_HANDLE;
	m_probeImg = R::GxImage();
	for (Pool& p : m_pools)
		vkDestroyDescriptorPool(dev, p.pool, nullptr);   // the device is idle at teardown
	m_pools.clear();
	if (m_pipeline)
		vkDestroyPipeline(dev, m_pipeline, nullptr);
	if (m_layout)
		vkDestroyPipelineLayout(dev, m_layout, nullptr);
	if (m_setLayout)
		vkDestroyDescriptorSetLayout(dev, m_setLayout, nullptr);
	if (m_sampler)
		vkDestroySampler(dev, m_sampler, nullptr);
	for (VkShaderModule m : { m_vs, m_fs })
		if (m)
			vkDestroyShaderModule(dev, m, nullptr);
	m_pipeline = VK_NULL_HANDLE;
	m_layout = VK_NULL_HANDLE;
	m_setLayout = VK_NULL_HANDLE;
	m_sampler = VK_NULL_HANDLE;
	m_vs = m_fs = VK_NULL_HANDLE;
	m_ctx = nullptr;
}

GxSceneCopy::Tap GxSceneCopy::prepare(R::GxResources& res, uint32_t needs, uint32_t depthFmt, uint32_t dw, uint32_t dh,
	uint32_t colourFmt, uint32_t cw, uint32_t ch, std::vector<R::GxRenderTarget*>* old, std::string* logLine)
{
	Tap t;
	t.needs = needs;
	t.fallbackNothing = m_plants.fallbackRecordsNothing;
	struct Want { bool on; uint32_t fmt, w, h; std::unique_ptr<R::GxRenderTarget>* slot; int i; const char* what; };
	const Want wants[2] = { { (needs & GX_SCENE_DEPTH) && depthFmt, depthFmt, dw, dh, &m_depthRt, 0, "depth" },
		{ (needs & GX_SCENE_COLOUR) && colourFmt, colourFmt, cw, ch, &m_colourRt, 1, "colour" } };
	for (const Want& k : wants) {
		if (!k.on)
			continue;
		std::unique_ptr<R::GxRenderTarget>& rt = *k.slot;
		if (!rt || m_fmt[k.i] != k.fmt || rt->image().width != k.w || rt->image().height != k.h) {
			if (m_failedFmt[k.i] == k.fmt)
				continue;   // failed once for this format: logged then, not retried at every tap
			if (rt)
				old->push_back(rt.release());
			m_fmt[k.i] = 0;
			R::RenderTargetDesc nd;
			nd.width = k.w;
			nd.height = k.h;
			nd.format = k.fmt;
			nd.samples = 1;
			nd.depthSampled = k.i == 0;   // + a DEPTH-aspect view; a colour target is SAMPLED anyway at one sample
			auto n = std::make_unique<R::GxRenderTarget>();
			std::string why;
			const bool ok = n->init(res, nd, &why) && (k.i == 1 || n->depthSampleView() != VK_NULL_HANDLE);
			if (!ok) {
				if (n->image().image)
					old->push_back(n.release());   // made but not sampleable: the exec half frees it like a replaced one
				m_failedFmt[k.i] = k.fmt;
				++t.refused;
				*logLine += std::string(*logLine == "" ? "" : "; ") + "[GXFDEPTH] no " + k.what + " copy for d3d format " +
					std::to_string(k.fmt) + ": " + (why.empty() ? "the format cannot be sampled" : why);
				continue;
			}
			char line[160];
			std::snprintf(line, sizeof(line), "[GXFDEPTH] %s copy %ux%u %s usage=0x%x", k.what, k.w, k.h,
				n->image().format ? n->image().format->name : "?", unsigned(n->image().usage));
			*logLine += std::string(*logLine == "" ? "" : "; ") + line;
			rt = std::move(n);
			m_fmt[k.i] = k.fmt;
		}
		(k.i == 0 ? t.depth : t.colour) = rt.get();
	}
	return t;
}

// The copies the world's own scopes did not resolve: an own pass (multisample depth), vkCmdResolveImage (multisample colour) or
// vkCmdCopyImage (single sample). Outside any scope; every barrier through the writer's tracker.
bool GxSceneCopy::recordFallback(VkCommandBuffer cmd, Rc::GxImageTracker& tr, uint32_t kinds, const Tap& t, R::GxImage* wc,
	R::GxImage* wd, VkImageView wdView, uint8_t* depthPath, uint8_t* colourPath)
{
	if (t.fallbackNothing) {   // the plant: published, never written - with the paths the real work would report, so its X- arms
		// detect it by the pixels the probe reads, never by a path label
		*depthPath = !(kinds & GX_SCENE_DEPTH) ? GXSP_NONE : wd->samples != VK_SAMPLE_COUNT_1_BIT ? GXSP_PASS : GXSP_COPY;
		*colourPath = !(kinds & GX_SCENE_COLOUR) ? GXSP_NONE : wc->samples != VK_SAMPLE_COUNT_1_BIT ? GXSP_PASS : GXSP_COPY;
		return true;
	}
	R::GxImage* dt = (kinds & GX_SCENE_DEPTH) ? &t.depth->image() : nullptr;
	R::GxImage* ct = (kinds & GX_SCENE_COLOUR) ? &t.colour->image() : nullptr;
	const bool depthMs = dt && wd->samples != VK_SAMPLE_COUNT_1_BIT;
	if (dt) {
		if (depthMs) {
			tr.need(*wd, Rc::Use::depthAttachment(false));
			tr.need(*dt, Rc::Use::resolveTarget(true));
		} else {
			tr.need(*wd, Rc::Use::transferSrc());
			tr.need(*dt, Rc::Use::transferDst(true));
		}
	}
	if (ct) {
		tr.need(*wc, Rc::Use::transferSrc());
		tr.need(*ct, Rc::Use::transferDst(true));
	}
	tr.flush(cmd);
	// everything recorded here reads or writes these in THIS submission: none may be retired before it completes
	const uint64_t use = m_res->nextUseValue();
	for (R::GxImage* i : { wd, dt, wc, ct })
		if (i)
			i->markUsed(use);
	if (dt && depthMs) {
		// the world's own load + store, plus the resolve: no draw (one tile load and store of the multisample depth)
		VkRenderingAttachmentInfo da = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
		da.imageView = wdView;
		da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
		da.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
		da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		da.resolveMode = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
		da.resolveImageView = t.depth->attachmentView();
		da.resolveImageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
		VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO };
		ri.renderArea = { { 0, 0 }, { wd->width, wd->height } };
		ri.layerCount = 1;
		ri.pDepthAttachment = &da;
		if (wd->format && wd->format->stencil)
			ri.pStencilAttachment = &da;
		vkCmdBeginRendering(cmd, &ri);
		vkCmdEndRendering(cmd);
		*depthPath = GXSP_PASS;
	} else if (dt) {
		VkImageCopy c = {};
		c.srcSubresource = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1 };   // the readers sample depth only
		c.dstSubresource = c.srcSubresource;
		c.extent = { wd->width, wd->height, 1 };
		vkCmdCopyImage(cmd, wd->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dt->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
		*depthPath = GXSP_COPY;
	}
	if (ct) {
		if (wc->samples != VK_SAMPLE_COUNT_1_BIT) {
			VkImageResolve r = {};
			r.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
			r.dstSubresource = r.srcSubresource;
			r.extent = { wc->width, wc->height, 1 };
			vkCmdResolveImage(cmd, wc->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, ct->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
			*colourPath = GXSP_PASS;
		} else {
			VkImageCopy c = {};
			c.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
			c.dstSubresource = c.srcSubresource;
			c.extent = { wc->width, wc->height, 1 };
			vkCmdCopyImage(cmd, wc->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, ct->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
			*colourPath = GXSP_COPY;
		}
	}
	return true;
}

void GxSceneCopy::tap(Rc::GxPacketWriter& w, const Tap& t, R::GxImage* wc, R::GxImage* wd, VkImageView wdView)
{
	++m_stats.taps;
	m_stats.refused += t.refused;
	m_frame.depth = m_frame.colour = false;
	m_frame.depthPath = m_frame.colourPath = GXSP_NONE;
	// what this frame can take: a target of the bound image's own format and size (the front made it from the same binding)
	const bool wantD = (t.needs & GX_SCENE_DEPTH) && t.depth && wd && wdView && t.depth->depthSampleView() &&
		t.depth->image().format == wd->format && t.depth->image().width == wd->width && t.depth->image().height == wd->height;
	const bool wantC = (t.needs & GX_SCENE_COLOUR) && t.colour && wc && t.colour->image().format == wc->format &&
		t.colour->image().width == wc->width && t.colour->image().height == wc->height;
	// (1) what the world's own scopes resolved since the last Present - only when armed for exactly these images
	uint32_t held = w.suspendScopeResolve(wc);   // also ends the open scope: the resolve runs there
	if (m_armDepthAtt != wd || m_armDepth != (wantD ? &t.depth->image() : nullptr) || m_armColour != (wantC ? &t.colour->image() : nullptr))
		held = 0;
	uint32_t done = 0;
	if (wantD && (held & 1u)) {
		done |= GX_SCENE_DEPTH;
		m_frame.depthPath = GXSP_IN_SCOPE;
	}
	if (wantC && (held & 2u)) {
		done |= GX_SCENE_COLOUR;
		m_frame.colourPath = GXSP_IN_SCOPE;
	}
	if (done)
		++m_stats.inScope;
	// (2)/(3) the rest, recorded now
	const uint32_t rest = ((wantD ? GX_SCENE_DEPTH : 0u) | (wantC ? GX_SCENE_COLOUR : 0u)) & ~done;
	if (rest) {
		bool ok = false;
		uint8_t dp = GXSP_NONE, cp = GXSP_NONE;
		w.outsideScope([&](VkCommandBuffer cmd) { ok = recordFallback(cmd, w.tracker(), rest, t, wc, wd, wdView, &dp, &cp); }, nullptr, 0);
		if (ok) {
			done |= rest;
			// S7 A1 (integrator 12:1xZ): the copies were written by this frame's MAIN commands - a shadow caster that
			// samples one must not be replayed from INIT, which runs before them
			if (rest & GX_SCENE_DEPTH)
				t.depth->image().mainWrite = w.frameValue();
			if (rest & GX_SCENE_COLOUR)
				t.colour->image().mainWrite = w.frameValue();
			if (rest & GX_SCENE_DEPTH)
				m_frame.depthPath = dp;
			if (rest & GX_SCENE_COLOUR)
				m_frame.colourPath = cp;
			if (dp == GXSP_PASS || cp == GXSP_PASS)
				++m_stats.passes;
			if (dp == GXSP_COPY || cp == GXSP_COPY)
				++m_stats.copies;
		}
	}
	// to the readers: one barrier now (outside any scope - the suspension or the outside work ended it), none in their draws
	if (done & GX_SCENE_DEPTH)
		w.tracker().need(t.depth->image(), Rc::Use::sampled());
	if (done & GX_SCENE_COLOUR)
		w.tracker().need(t.colour->image(), Rc::Use::sampled());
	if (w.tracker().pending())
		w.outsideScope([](VkCommandBuffer) {}, nullptr, 0);   // flushes the tracker at an outside point (no scope is open now)
	// (1) for the frames after this one: the world's multisample scopes carry the resolve (never forced off, never single-sample)
	Rc::GxScopeResolve r;
	if (!m_forcePass && wd && wd->samples != VK_SAMPLE_COUNT_1_BIT && (wantD || wantC)) {
		r.depthAtt = wd;
		if (wantD) {
			r.depthTarget = &t.depth->image();
			r.depthView = t.depth->attachmentView();
		}
		if (wantC && wc->samples != VK_SAMPLE_COUNT_1_BIT) {
			r.colourTarget = &t.colour->image();
			r.colourView = t.colour->attachmentView();
		}
		w.setScopeResolve(&r);
	} else
		w.setScopeResolve(nullptr);
	m_armDepthAtt = r.depthAtt;
	m_armDepth = r.depthTarget;
	m_armColour = r.colourTarget;
	// publish
	m_frame.depth = (done & GX_SCENE_DEPTH) != 0;
	m_frame.colour = (done & GX_SCENE_COLOUR) != 0;
	m_frame.depthImage = m_frame.depth ? &t.depth->image() : nullptr;
	m_frame.depthView = m_frame.depth ? t.depth->depthSampleView() : VK_NULL_HANDLE;
	m_frame.colourImage = m_frame.colour ? &t.colour->image() : nullptr;
	m_frame.colourView = m_frame.colour ? t.colour->attachmentView() : VK_NULL_HANDLE;
	m_frame.width = m_frame.depth ? wd->width : m_frame.colour ? wc->width : 0;
	m_frame.height = m_frame.depth ? wd->height : m_frame.colour ? wc->height : 0;
	++m_frame.serial;
	if (m_probeNeeds && !m_probeAtPresent && (m_frame.depth || m_frame.colour))
		recordProbe(w);
}

void GxSceneCopy::presented(Rc::GxPacketWriter& w)
{
	if (m_probeNeeds && m_probeAtPresent && (m_frame.depth || m_frame.colour) && w.frameOpen())
		recordProbe(w);   // TESTS: after everything the frame drew on top of the tap
	m_frame.depth = m_frame.colour = false;
	w.resumeScopeResolve();
}

void GxSceneCopy::disarm(Rc::GxPacketWriter& w)
{
	w.setScopeResolve(nullptr);
	w.resumeScopeResolve();
	m_armDepthAtt = m_armDepth = m_armColour = nullptr;
	m_frame.depth = m_frame.colour = false;
}

VkDescriptorSet GxSceneCopy::probeSet(VkImageView a, VkImageView b)
{
	const VkDevice dev = m_ctx->device();
	const uint64_t done = m_res->completed();
	Pool* pool = nullptr;
	for (Pool& p : m_pools)
		if (!pool && p.lastUse <= done) {
			vkResetDescriptorPool(dev, p.pool, 0);
			pool = &p;
		}
	if (!pool) {
		VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 };
		VkDescriptorPoolCreateInfo pc = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
		pc.maxSets = 1;
		pc.poolSizeCount = 1;
		pc.pPoolSizes = &ps;
		Pool p;
		if (vkCreateDescriptorPool(dev, &pc, nullptr, &p.pool) != VK_SUCCESS)
			return VK_NULL_HANDLE;
		m_pools.push_back(p);
		pool = &m_pools.back();
	}
	pool->lastUse = m_res->nextUseValue();
	VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
	ai.descriptorPool = pool->pool;
	ai.descriptorSetCount = 1;
	ai.pSetLayouts = &m_setLayout;
	VkDescriptorSet set = VK_NULL_HANDLE;
	if (vkAllocateDescriptorSets(dev, &ai, &set) != VK_SUCCESS)
		return VK_NULL_HANDLE;
	const VkDescriptorImageInfo ii[2] = { { m_sampler, a, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
		{ m_sampler, b, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL } };
	VkWriteDescriptorSet wr[2] = { { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET }, { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET } };
	for (uint32_t i = 0; i < 2; ++i) {
		wr[i].dstSet = set;
		wr[i].dstBinding = i;
		wr[i].descriptorCount = 1;
		wr[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		wr[i].pImageInfo = &ii[i];
	}
	vkUpdateDescriptorSets(dev, 2, wr, 0, nullptr);
	return set;
}

// The dummy reader: one fragment, one texelFetch of each copy it asked for. Its own scope on its own 1x1 target - a real reader
// samples inside the world's scope instead; this is the floor of what any reader adds. No GX_GPUTIME scope of its own: the timer
// sums every named scope into present= (render-motion-2's a31 note), so its cost shows in the frame total only.
void GxSceneCopy::recordProbe(Rc::GxPacketWriter& w)
{
	const bool d = (m_probeNeeds & GX_SCENE_DEPTH) && m_frame.depth;
	const bool c = (m_probeNeeds & GX_SCENE_COLOUR) && m_frame.colour;
	if (!m_pipeline || !m_probeView || (!d && !c))
		return;
	const VkImageView a = d ? m_frame.depthView : m_frame.colourView;
	const VkImageView b = c ? m_frame.colourView : m_frame.depthView;
	R::GxImage* da = d ? m_frame.depthImage : m_frame.colourImage;
	R::GxImage* db = c ? m_frame.colourImage : m_frame.depthImage;
	const int32_t push[4] = { m_probeX >= 0 ? m_probeX : int32_t(m_frame.width / 2), m_probeY >= 0 ? m_probeY : int32_t(m_frame.height / 2),
		d ? 1 : 0, c ? 1 : 0 };
	w.outsideScope([&](VkCommandBuffer cmd) {
		Rc::GxImageTracker& tr = w.tracker();
		tr.need(*da, Rc::Use::sampled());   // already there since the tap: no barrier
		tr.need(*db, Rc::Use::sampled());
		tr.need(m_probeImg, Rc::Use::colorAttachment(true));
		tr.flush(cmd);
		const VkDescriptorSet set = probeSet(a, b);
		if (!set)
			return;
		VkRenderingAttachmentInfo ca = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
		ca.imageView = m_probeView;
		ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		ca.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO };
		ri.renderArea = { { 0, 0 }, { 1, 1 } };
		ri.layerCount = 1;
		ri.colorAttachmentCount = 1;
		ri.pColorAttachments = &ca;
		vkCmdBeginRendering(cmd, &ri);
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);
		const VkViewport vp = { 0.f, 0.f, 1.f, 1.f, 0.f, 1.f };
		const VkRect2D sc = { { 0, 0 }, { 1, 1 } };
		vkCmdSetViewport(cmd, 0, 1, &vp);
		vkCmdSetScissor(cmd, 0, 1, &sc);
		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_layout, 0, 1, &set, 0, nullptr);
		vkCmdPushConstants(cmd, m_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), push);
		vkCmdDraw(cmd, 3, 1, 0, 0);
		vkCmdEndRendering(cmd);
		const uint64_t v = m_res->nextUseValue();
		m_probeImg.markUsed(v);
		m_probeImg.mainWrite = w.frameValue();   // S7 A1: written in MAIN
		da->markUsed(v);
		db->markUsed(v);
		++m_stats.probes;
	}, nullptr, 0);
}

}} // namespace GXRender::Dev
