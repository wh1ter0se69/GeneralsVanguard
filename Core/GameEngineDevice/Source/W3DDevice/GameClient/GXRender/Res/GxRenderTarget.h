// GeneralsX: WS-C GXRES v0: render-target and depth-stencil SURFACES (D3D8 CreateRenderTarget /
// CreateDepthStencilSurface / the implicit back buffer and auto depth). Render-target TEXTURES are GxTexture with
// d3d::USAGE_RENDERTARGET (water reflection 256x256, pillarbox offscreen, shader-manager filter target - CENSUS §4).
//
// Sample counts follow DXVK (d3d9_util.cpp DecodeMultiSampleType): the D3D8 count is reduced to the highest the device
// supports for BOTH colour and depth (framebuffer*SampleCounts), never refused - so a native MSAA x8 frame has the sample
// count the CLASSIC path really used on that device. resolveTo() is D3D8's CopyRects(MSAA surface -> texture) at Pillarbox_End
// (dx8wrapper.cpp:480), the "MSAA resolved BEFORE post" slot of PLAN §1b #1.
#pragma once
#include "GxResources.h"

namespace GXRender { namespace Res {

struct RenderTargetDesc
{
	uint32_t width = 0, height = 0;
	uint32_t format = 0;     // d3d:: colour or depth format
	uint32_t samples = 0;    // D3D8 multisample type: 0/1 = none, 2..16 = samples
	bool sampled = false;    // MSAA colour ALSO sampled (the native device's present reads the back buffer's samples - DXVK's
	                         // linear-light resolve, 11/10); init fails when the device cannot sample that count
	bool depthSampled = false;   // 13/10: a single-sample DEPTH target also gets SAMPLED + a DEPTH-aspect view
	                             // (the motion key); false = usage bit-identical to before
};

class GxRenderTarget
{
public:
	GxRenderTarget() = default;
	~GxRenderTarget() { destroy(); }
	GxRenderTarget(const GxRenderTarget&) = delete;
	GxRenderTarget& operator=(const GxRenderTarget&) = delete;

	bool init(GxResources& res, const RenderTargetDesc& desc, std::string* why);
	void destroy();

	GxImage& image() { return m_image; }
	VkImageView attachmentView() const { return m_view; }
	VkImageView depthSampleView() const { return m_depthView; }   // DEPTH aspect only; null unless desc.depthSampled took
	bool depth() const { return m_image.format && m_image.format->depth; }
	VkSampleCountFlagBits samples() const { return m_image.samples; }
	uint32_t requestedSamples() const { return m_desc.samples; }
	bool sampled() const { return m_desc.sampled; }   // an MSAA colour target that shaders may read (render-hud-seam-1 ResolveLinear)

	// -> COLOR_ATTACHMENT_OPTIMAL or DEPTH_STENCIL_ATTACHMENT_OPTIMAL, for the pass recorder (WS-F1) and the tests.
	void beginWrite(VkCommandBuffer cmd);
	// MSAA colour -> a single-sample image of the same format (vkCmdResolveImage, outside a render pass). dst ends in
	// SHADER_READ_ONLY_OPTIMAL. Same-size only (the pillarbox resolve is 1:1).
	bool resolveTo(VkCommandBuffer cmd, GxImage& dst);
	// Single-sample colour only (GX_SHOT / screenshot read-back): synchronous GPU -> host, tight rows.
	bool readback(std::vector<uint8_t>* out);
	// The sample count DXVK would use for a D3D8 multisample type on this device.
	// maxSamples: FormatCaps::maxSamples (0 = no extra cap) - a forced reduced-capability device.
	static VkSampleCountFlagBits clampSamples(const VkPhysicalDeviceLimits& limits, uint32_t d3dSamples, uint32_t maxSamples = 0);
	// The largest power of two <= cap, at most 64 (Vulkan's largest count): a huge cap would wrap a doubling loop.
	static uint32_t capPow2(uint32_t cap);

private:
	GxResources* m_res = nullptr;
	RenderTargetDesc m_desc;
	GxImage m_image;
	VkImageView m_view = VK_NULL_HANDLE;
	VkImageView m_depthView = VK_NULL_HANDLE;
};

}} // namespace GXRender::Res
