// GeneralsX @feature D · look-shadowmaps 17/10/2026 S7 GPU sun shadow map: the caster list and its INIT replay (see the header).
#include "GxShadowCasters.h"
#include "GxLookFrame.h"
#include "GxOwnPull.h"

#include "GxResources.h"
#include "VkCtx.h"
#include "shaders/gx_shadow_caster.vert.h"

#include <cmath>
#include <cstring>

namespace GXRender { namespace Rec {

namespace {
// row-vector product A x B, D3D element order (_11 _12 ... = a[row * 4 + col])
void mul4(const float* a, const float* b, float* out)
{
	for (int r = 0; r < 4; ++r)
		for (int c = 0; c < 4; ++c) {
			float s = 0.f;
			for (int k = 0; k < 4; ++k)
				s += a[r * 4 + k] * b[k * 4 + c];
			out[r * 4 + c] = s;
		}
}
} // namespace

bool GxShadowCasters::init(Vk::VkCtx& ctx, VkFormat depthFormat, uint32_t capacity, std::string* why)
{
	shutdown();
	m_ctx = &ctx;
	m_depthFormat = depthFormat;
	m_capacity = capacity;
	m_records.reserve(capacity);
	m_maxStride = ctx.limits().maxVertexInputBindingStride;
	m_biasClampFeature = ctx.features().depthBiasClamp;
	VkDevice dev = ctx.device();

	VkPushConstantRange pcr = { VK_SHADER_STAGE_VERTEX_BIT, 0, 64 };
	VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
	pli.pushConstantRangeCount = 1;
	pli.pPushConstantRanges = &pcr;
	if (vkCreatePipelineLayout(dev, &pli, nullptr, &m_layout) != VK_SUCCESS) {
		*why = "shadow caster pipeline layout";
		return false;
	}
	VkShaderModuleCreateInfo mi = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
	mi.codeSize = sizeof(spv_gx_shadow_caster_vert);
	mi.pCode = spv_gx_shadow_caster_vert;
	if (vkCreateShaderModule(dev, &mi, nullptr, &m_vs) != VK_SUCCESS) {
		*why = "shadow caster vertex module";
		return false;
	}
	VkPipelineShaderStageCreateInfo st = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
	st.stage = VK_SHADER_STAGE_VERTEX_BIT;
	st.module = m_vs;
	st.pName = "main";
	// position = float3 at the bound offset; the stride is dynamic (vkCmdBindVertexBuffers2), as in every FF pipeline
	VkVertexInputBindingDescription vb = { 0, 12, VK_VERTEX_INPUT_RATE_VERTEX };
	VkVertexInputAttributeDescription va = { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0 };
	VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
	vi.vertexBindingDescriptionCount = 1;
	vi.pVertexBindingDescriptions = &vb;
	vi.vertexAttributeDescriptionCount = 1;
	vi.pVertexAttributeDescriptions = &va;
	VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
	ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;   // dynamic: list / strip (VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY, same class)
	VkPipelineViewportStateCreateInfo vps = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
	vps.viewportCount = 1;
	vps.scissorCount = 1;
	VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
	// casters nearer the sun than the fit's near plane still cast: pancake them onto it when the device can (A28 also widens
	// the depth range to the walk's runtime caster bounds, so this is the second line of defence)
	rs.depthClampEnable = ctx.features().depthClamp ? VK_TRUE : VK_FALSE;
	rs.polygonMode = VK_POLYGON_MODE_FILL;
	rs.cullMode = VK_CULL_MODE_NONE;          // W3D meshes are not closed: both faces cast
	rs.depthBiasEnable = VK_TRUE;
	rs.lineWidth = 1.f;
	VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
	ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
	VkPipelineDepthStencilStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
	ds.depthTestEnable = VK_TRUE;
	ds.depthWriteEnable = VK_TRUE;
	ds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
	VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
	const VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_DEPTH_BIAS,
		VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE, VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY };
	VkPipelineDynamicStateCreateInfo dy = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
	dy.dynamicStateCount = uint32_t(sizeof(dyn) / sizeof(dyn[0]));
	dy.pDynamicStates = dyn;
	VkPipelineRenderingCreateInfo rci = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
	rci.depthAttachmentFormat = depthFormat;
	VkGraphicsPipelineCreateInfo gp = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
	gp.pNext = &rci;
	gp.stageCount = 1;                        // depth only: no fragment stage
	gp.pStages = &st;
	gp.pVertexInputState = &vi;
	gp.pInputAssemblyState = &ia;
	gp.pViewportState = &vps;
	gp.pRasterizationState = &rs;
	gp.pMultisampleState = &ms;
	gp.pDepthStencilState = &ds;
	gp.pColorBlendState = &cb;
	gp.pDynamicState = &dy;
	gp.layout = m_layout;
	if (vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gp, nullptr, &m_opaque) != VK_SUCCESS) {
		*why = "shadow caster pipeline (opaque)";
		return false;
	}
	return true;
}

void GxShadowCasters::shutdown()
{
	if (!m_ctx)
		return;
	VkDevice dev = m_ctx->device();
	// Codex code-p0b-r1 P2: a replay recorded into a submitted INIT buffer may still be executing - nothing here is destroyed
	// before the GPU is idle (shutdown is device teardown or a test: never per frame)
	if (m_opaque)
		m_ctx->waitIdle();
	if (m_opaque)
		vkDestroyPipeline(dev, m_opaque, nullptr);
	if (m_vs)
		vkDestroyShaderModule(dev, m_vs, nullptr);
	if (m_layout)
		vkDestroyPipelineLayout(dev, m_layout, nullptr);
	m_opaque = VK_NULL_HANDLE;
	m_vs = VK_NULL_HANDLE;
	m_layout = VK_NULL_HANDLE;
	m_records.clear();
	m_ctx = nullptr;
}

void GxShadowCasters::beginFrame(uint64_t frameValue)
{
	m_frame = frameValue;
	m_records.clear();
}

bool GxShadowCasters::capture(const GxCasterRecord& r, bool castOnly, bool lateBuf, bool lateTex)
{
	if (!active())
		return false;
	if (lateBuf) {
		++m_stats.skippedLateBuf;
		return false;
	}
	if (lateTex) {
		++m_stats.skippedLateTex;
		return false;
	}
	if (m_records.size() >= m_capacity) {
		++m_stats.overflow;
		if (!m_overflowLogged && m_ctx) {
			m_ctx->logf(Vk::LogLevel::Warn, "[GXSHADOWMAP] caster list full (%u): later casters this frame are dropped and counted",
				m_capacity);
			m_overflowLogged = true;
		}
		return false;
	}
	m_records.push_back(r);
	++m_stats.captured;
	if (castOnly)
		++m_stats.castOnly;
	return true;
}

void GxShadowCasters::record(VkCommandBuffer cmd)
{
	if (!active() || !cmd || !m_opaque) {
		m_records.clear();
		return;
	}
	Res::GxImage& atlas = *m_pass.atlas;
	atlas.transition(cmd, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
		VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
		VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
	const int replays = debug.replayTwice ? 2 : 1;
	// A73 (Codex a71-r1 #1): a non-zero clamp needs the device's depthBiasClamp (VUID-vkCmdSetDepthBias-depthBiasClamp-00790). Without it
	// the clamp is dropped - 0 = unclamped, the pre-A73 bias: valid, airborne casters' shadows checkered - counted, and logged once
	float clamp = m_pass.depthBias[1];
	if (clamp != 0.f && (!m_biasClampFeature || debug.noBiasClampFeature) && !debug.noBiasGuard) {
		if (m_stats.biasClampDropped++ == 0)
			m_ctx->logf(Vk::LogLevel::Warn, "[GXSHADOWMAP] the device lacks depthBiasClamp: the caster pass's bias clamp %.8f is dropped (unclamped)",
				double(clamp));
		clamp = 0.f;
	}
	// A79 (E · look-shadowmaps-7, GxOwnPull.h): a thin own-light copy (0 < mu <= muMax) is replayed pulled toward L - world x (P x lightVP[c]),
	// P x lightVP built once per cascade; every other record keeps world x lightVP[c]. eps 0 = no pull (key default until D79's policy)
	const float eps = m_pass.ownPull[3], muMax = m_pass.ownPullAux[0];
	float pull[16];
	gxOwnPullMatrix(m_pass.ownPull, eps, pull);
	// A92: the cascades into one target - the atlas (copy 0) or replay-k's scratch (copies 1..k-1, TEST-ONLY)
	auto replayInto = [&](VkImageView view, uint32_t copy) {
		for (uint32_t c = 0; c < m_pass.cascades && c < 2; ++c) {
			const VkRect2D& rc = m_pass.rect[c];
			float pulledVP[16];
			gxOwnPullViewProj(pull, m_pass.lightViewProj[c], pulledVP);
			if ((c > 0 || copy > 0) && !debug.noCascadeBarrier) {   // A92: also between replay-k copies on the scratch image
				// Codex code-p0b-r1 P1: the next cascade's rendering instance (its CLEAR load op) on the SAME atlas subresource is
				// ordered after the previous one's depth writes and store - disjoint render areas are not a synchronization guarantee
				VkMemoryBarrier2 mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
				mb.srcStageMask = mb.dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
				mb.srcAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
				mb.dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
				VkDependencyInfo di = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
				di.memoryBarrierCount = 1;
				di.pMemoryBarriers = &mb;
				vkCmdPipelineBarrier2(cmd, &di);
			}
			VkRenderingAttachmentInfo da = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
			da.imageView = view;
			da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
			da.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;   // clears the render area = this cascade's rect only
			da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
			da.clearValue.depthStencil = { 1.f, 0 };
			VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO };
			ri.renderArea = rc;
			ri.layerCount = 1;
			ri.pDepthAttachment = &da;
			vkCmdBeginRendering(cmd, &ri);
			++m_stats.passes;
			if (!m_records.empty() && !(copy > 0 && m_replayNoDraw)) {   // A92 NODRAW plant: copies keep their clears, lose their draws
				vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_opaque);
				VkViewport vp = { float(rc.offset.x), float(rc.offset.y), float(rc.extent.width), float(rc.extent.height), 0.f, 1.f };
				vkCmdSetViewport(cmd, 0, 1, &vp);
				vkCmdSetScissor(cmd, 0, 1, &rc);
				vkCmdSetDepthBias(cmd, m_pass.depthBias[0], clamp, m_pass.depthBias[2]);
				m_stats.lastBias[0] = m_pass.depthBias[0];
				m_stats.lastBias[1] = clamp;
				m_stats.lastBias[2] = m_pass.depthBias[2];
				VkBuffer lastVb = VK_NULL_HANDLE, lastIb = VK_NULL_HANDLE;
				VkDeviceSize lastVbOff = ~VkDeviceSize(0), lastIbOff = ~VkDeviceSize(0);
				VkIndexType lastType = VK_INDEX_TYPE_MAX_ENUM;
				uint32_t lastStride = ~0u;
				VkPrimitiveTopology lastTopo = VK_PRIMITIVE_TOPOLOGY_MAX_ENUM;
				for (int rep = 0; rep < replays; ++rep)
					for (const GxCasterRecord& r : m_records) {
						if (r.vb != lastVb || r.vbOffset != lastVbOff || r.stride != lastStride) {
							const VkDeviceSize stride = r.stride;
							vkCmdBindVertexBuffers2(cmd, 0, 1, &r.vb, &r.vbOffset, nullptr, &stride);
							lastVb = r.vb;
							lastVbOff = r.vbOffset;
							lastStride = r.stride;
						}
						if (r.topology != lastTopo) {
							vkCmdSetPrimitiveTopology(cmd, r.topology);
							lastTopo = r.topology;
						}
						float m[16];
						const bool pulled = debug.pullAll || gxOwnPullApplies(r.ownMu, muMax, eps);
						mul4(r.world, pulled ? pulledVP : m_pass.lightViewProj[c], m);
						if (pulled)
							++m_stats.pulledDraws;
						vkCmdPushConstants(cmd, m_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(m), m);
						if (r.indexed) {
							if (r.ib != lastIb || r.ibOffset != lastIbOff || r.indexType != lastType) {
								vkCmdBindIndexBuffer(cmd, r.ib, r.ibOffset, r.indexType);
								lastIb = r.ib;
								lastIbOff = r.ibOffset;
								lastType = r.indexType;
							}
							vkCmdDrawIndexed(cmd, r.count, 1, r.first, r.vertexOffset, 0);
						} else
							vkCmdDraw(cmd, r.count, 1, r.first, 0);
						++m_stats.replayedDraws;
					}
			}
			vkCmdEndRendering(cmd);
		}
	};
	replayInto(m_pass.attachmentView, 0);
	if (m_replayK > 1) {
		m_scratch->transition(cmd, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
			VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
			VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
		for (uint32_t copy = 1; copy < m_replayK; ++copy)
			replayInto(m_scratchView, copy);
		m_scratch->markUsed(m_frame);
	}
	atlas.transition(cmd, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
		VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
	atlas.markUsed(m_frame);
	++m_stats.frames;
	m_records.clear();
}

// P2 (GxLookFrame.h): the receivers' view of the frame's pass. The uv mapping is the replay's own viewport (offset rect.xy, size
// rect.wh, positive height: clip y +1 = the rect's bottom row), so a receiver lands on the texel its caster was drawn into.
void GxPackLookFrame(const GxShadowPassDesc& p, uint32_t atlasW, uint32_t atlasH, GxLookFrameUbo* o)
{
	std::memset(o, 0, sizeof(*o));
	const bool on = p.cascades > 0 && p.atlas != nullptr && p.step < 3 && atlasW > 0 && atlasH > 0;
	o->mode[0] = on ? p.step : 3u;
	o->mode[1] = on ? p.cascades : 0u;
	o->mode[2] = p.filter;
	const float W = float(atlasW ? atlasW : 1), H = float(atlasH ? atlasH : 1);
	for (int c = 0; c < 2; ++c) {
		std::memcpy(o->lightVP[c], p.lightViewProj[c], sizeof(o->lightVP[c]));
		const float x = float(p.rect[c].offset.x), y = float(p.rect[c].offset.y);
		const float w = float(p.rect[c].extent.width), h = float(p.rect[c].extent.height);
		o->uvMap[c][0] = 0.5f * w / W;
		o->uvMap[c][1] = 0.5f * h / H;
		o->uvMap[c][2] = (x + 0.5f * w) / W;
		o->uvMap[c][3] = (y + 0.5f * h) / H;
		o->uvClamp[c][0] = (x + 0.5f) / W;
		o->uvClamp[c][1] = (y + 0.5f) / H;
		o->uvClamp[c][2] = (x + w - 0.5f) / W;
		o->uvClamp[c][3] = (y + h - 0.5f) / H;
		// the fit keeps every receiver inside 1 - 2 (guard + 1) / res of the half-extent; the lookup's square stops `guard` texels in
		o->limits[c] = w > 0.f ? 1.f - 2.f * float(p.guard) / w : 1.f;
		// A39: a perspective pass (far > 0) gives tangent texels and near / far; an orthographic one (the test frames) world-unit texels and
		// its depth range, as before
		const bool persp = p.nearFar[c][1] > 0.f;
		o->texel[2 + c] = persp ? p.tanTexel[c] : p.texelWU[c];
		o->depthWU[c] = persp ? 0.f : p.depthRangeWU[c];
		o->nearFar[2 * c] = persp ? p.nearFar[c][0] : 0.f;
		o->nearFar[2 * c + 1] = persp ? p.nearFar[c][1] : 0.f;
	}
	const uint32_t sc = p.shadowColor;   // ARGB
	o->shadowColor[0] = float((sc >> 16) & 0xffu) / 255.f;
	o->shadowColor[1] = float((sc >> 8) & 0xffu) / 255.f;
	o->shadowColor[2] = float(sc & 0xffu) / 255.f;
	o->shadowColor[3] = float((sc >> 24) & 0xffu) / 255.f;
	o->limits[2] = 0.1f;   // A14: c0 and c1 overlap by 10 % with a blend
	o->limits[3] = 4.f;    // A10: Rmax = 4 texels at step 0
	o->texel[0] = 1.f / W;
	o->texel[1] = 1.f / H;
	o->depthWU[2] = 2.f * std::tan(0.5f * 3.14159265f / 180.f);   // A10: 2 tan(theta / 2), theta = 1 deg
	// A79: the receivers' pull (the same L, eps and muMax the replay used), only with a live map (else all 0, as the key-off words); the
	// TEST-ONLY noRecv (gx_native_test's frame only) writes eps 0
	if (on) {
		for (int i = 0; i < 3; ++i)
			o->ownPull[i] = p.ownPull[i];
		o->ownPull[3] = p.ownPullAux[1] != 0.f ? 0.f : p.ownPull[3];
		o->ownPullAux[0] = p.ownPullAux[0];
	}
}

}} // namespace GXRender::Rec
