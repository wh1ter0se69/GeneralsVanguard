// GeneralsX: WS-C GXRES v0: D3D8 format -> Vulkan format, per device (see GxFormat.h).
#include "GxFormat.h"
#include "GxD3D.h"
#include "GxDecode.h"
#include <algorithm>

namespace GXRender { namespace Res {

namespace {

constexpr VkComponentSwizzle R = VK_COMPONENT_SWIZZLE_R, G = VK_COMPONENT_SWIZZLE_G, ONE = VK_COMPONENT_SWIZZLE_ONE, ZERO = VK_COMPONENT_SWIZZLE_ZERO,
	ID = VK_COMPONENT_SWIZZLE_IDENTITY;

FormatInfo colour(uint32_t d3d, const char* name, VkFormat vk, uint16_t bytes, VkComponentMapping sw = {ID, ID, ID, ID})
{
	FormatInfo f;
	f.d3d = d3d;
	f.name = name;
	f.vk = vk;
	f.blockBytes = bytes;
	f.swizzle = sw;
	f.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
	return f;
}

FormatInfo block(uint32_t d3d, const char* name, VkFormat vk, uint16_t bytes)
{
	FormatInfo f = colour(d3d, name, vk, bytes);
	f.blockW = f.blockH = 4;
	return f;
}

FormatInfo depth(uint32_t d3d, const char* name, VkFormat vk, uint16_t bytes, bool stencil)
{
	FormatInfo f;
	f.d3d = d3d;
	f.name = name;
	f.vk = vk;
	f.blockBytes = bytes;
	f.depth = true;
	f.stencil = stencil;
	f.aspect = VK_IMAGE_ASPECT_DEPTH_BIT | (stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
	return f;
}

VkFormatFeatureFlags optimalFeatures(VkPhysicalDevice pd, VkFormat f)
{
	VkFormatProperties p = {};
	vkGetPhysicalDeviceFormatProperties(pd, f, &p);
	return p.optimalTilingFeatures;
}

} // namespace

void FormatTable::init(VkPhysicalDevice pd, const FormatCaps& caps)
{
	m_formats.clear();
	m_caps = caps;
	VkPhysicalDeviceProperties props = {};
	vkGetPhysicalDeviceProperties(pd, &props);
	m_maxDim = caps.maxDim ? std::min(caps.maxDim, props.limits.maxImageDimension2D) : props.limits.maxImageDimension2D;
	// d3d9_format.cpp ConvertFormatUnfixed, D3D8 subset. X8R8G8B8 / X1R5G5B5 read alpha as ONE; X4R4G4B4 is mapped WITHOUT
	// that swizzle there (stored nibbles read as alpha) - copied, not "fixed", so a sample matches the CLASSIC path.
	m_formats.push_back(colour(d3d::A8R8G8B8, "A8R8G8B8", VK_FORMAT_B8G8R8A8_UNORM, 4));
	m_formats.push_back(colour(d3d::X8R8G8B8, "X8R8G8B8", VK_FORMAT_B8G8R8A8_UNORM, 4, {ID, ID, ID, ONE}));
	m_formats.push_back(colour(d3d::R5G6B5, "R5G6B5", VK_FORMAT_R5G6B5_UNORM_PACK16, 2));
	m_formats.push_back(colour(d3d::X1R5G5B5, "X1R5G5B5", VK_FORMAT_A1R5G5B5_UNORM_PACK16, 2, {ID, ID, ID, ONE}));
	m_formats.push_back(colour(d3d::A1R5G5B5, "A1R5G5B5", VK_FORMAT_A1R5G5B5_UNORM_PACK16, 2));
	m_formats.push_back(colour(d3d::A4R4G4B4, "A4R4G4B4", VK_FORMAT_A4R4G4B4_UNORM_PACK16, 2));
	m_formats.push_back(colour(d3d::X4R4G4B4, "X4R4G4B4", VK_FORMAT_A4R4G4B4_UNORM_PACK16, 2));
	m_formats.push_back(colour(d3d::A8, "A8", VK_FORMAT_R8_UNORM, 1, {ZERO, ZERO, ZERO, R}));
	m_formats.push_back(colour(d3d::L8, "L8", VK_FORMAT_R8_UNORM, 1, {R, R, R, ONE}));
	m_formats.push_back(colour(d3d::A8L8, "A8L8", VK_FORMAT_R8G8_UNORM, 2, {R, R, R, G}));
	m_formats.push_back(colour(d3d::A4L4, "A4L4", VK_FORMAT_R4G4_UNORM_PACK8, 1, {G, G, G, R}));
	m_formats.push_back(colour(d3d::V8U8, "V8U8", VK_FORMAT_R8G8_SNORM, 2, {R, G, ONE, ONE}));
	m_formats.push_back(colour(d3d::Q8W8V8U8, "Q8W8V8U8", VK_FORMAT_R8G8B8A8_SNORM, 4));
	m_formats.push_back(colour(d3d::V16U16, "V16U16", VK_FORMAT_R16G16_SNORM, 4, {R, G, ONE, ONE}));
	m_formats.push_back(block(d3d::DXT1, "DXT1", VK_FORMAT_BC1_RGBA_UNORM_BLOCK, 8));
	m_formats.push_back(block(d3d::DXT2, "DXT2", VK_FORMAT_BC2_UNORM_BLOCK, 16));
	m_formats.push_back(block(d3d::DXT3, "DXT3", VK_FORMAT_BC2_UNORM_BLOCK, 16));
	m_formats.push_back(block(d3d::DXT4, "DXT4", VK_FORMAT_BC3_UNORM_BLOCK, 16));
	m_formats.push_back(block(d3d::DXT5, "DXT5", VK_FORMAT_BC3_UNORM_BLOCK, 16));
	// GXRES-private HD-art formats (not D3D8): BC7 on desktop GPUs, ASTC 4x4 on Apple's (PLAN §1b "+").
	m_formats.push_back(block(d3d::GX_BC7, "BC7", VK_FORMAT_BC7_UNORM_BLOCK, 16));
	m_formats.push_back(block(d3d::GX_ASTC_4x4, "ASTC4x4", VK_FORMAT_ASTC_4x4_UNORM_BLOCK, 16));
	// GXRES-private render target (not D3D8): the motion bridge's object-velocity image (D · render-motion-units-1), key on only.
	if (caps.rgba16f)
		m_formats.push_back(colour(d3d::GX_RGBA16F, "RGBA16F", VK_FORMAT_R16G16B16A16_SFLOAT, 8));
	m_formats.push_back(depth(d3d::D16, "D16", VK_FORMAT_D16_UNORM, 2, false));

	// d3d9_format.cpp: D24S8 needs DEPTH_STENCIL_ATTACHMENT and SAMPLED on D24_UNORM_S8_UINT, else D32_SFLOAT_S8_UINT (and
	// D24X8 -> D32_SFLOAT). Apple GPUs have no D24S8, so the CLASSIC path there already tests depth in 32-bit float.
	const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
	const bool d24 = (optimalFeatures(pd, VK_FORMAT_D24_UNORM_S8_UINT) & need) == need;
	m_depthFallback = !d24;
	m_formats.push_back(depth(d3d::D24S8, "D24S8", d24 ? VK_FORMAT_D24_UNORM_S8_UINT : VK_FORMAT_D32_SFLOAT_S8_UINT, d24 ? 4 : 8, true));
	m_formats.push_back(depth(d3d::D24X8, "D24X8", d24 ? VK_FORMAT_D24_UNORM_S8_UINT : VK_FORMAT_D32_SFLOAT, 4, false));
	if (d24)
		m_formats.back().aspect = VK_IMAGE_ASPECT_DEPTH_BIT; // D24X8 uses the depth aspect of a D24S8 image

	for (FormatInfo& f : m_formats) {
		f.features = optimalFeatures(pd, f.vk);
		const bool bc = f.vk >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && f.vk <= VK_FORMAT_BC7_SRGB_BLOCK;
		const bool astc = f.vk >= VK_FORMAT_ASTC_4x4_UNORM_BLOCK && f.vk <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK;
		if ((bc && caps.noBC) || (astc && caps.noASTC))
			f.features = 0; // forced: this device "cannot" sample it
	}
}

bool FormatTable::decodes(uint32_t d3dFormat) const
{
	return canDecode(d3dFormat) && get(d3dFormat) && !supports(d3dFormat, 0) && supports(d3d::A8R8G8B8, 0);
}

const FormatInfo* FormatTable::imageFormat(uint32_t d3dFormat) const
{
	return decodes(d3dFormat) ? get(d3d::A8R8G8B8) : get(d3dFormat);
}

uint32_t FormatTable::chooseHdFormat() const
{
	if (supports(d3d::GX_ASTC_4x4, 0))
		return d3d::GX_ASTC_4x4;
	return supports(d3d::GX_BC7, 0) ? uint32_t(d3d::GX_BC7) : 0u;
}

const FormatInfo* FormatTable::get(uint32_t d3dFormat) const
{
	for (const FormatInfo& f : m_formats)
		if (f.d3d == d3dFormat)
			return &f;
	return nullptr;
}

bool FormatTable::supports(uint32_t d3dFormat, uint32_t usage) const
{
	const FormatInfo* f = get(d3dFormat);
	if (!f)
		return false;
	VkFormatFeatureFlags need = 0;
	if (usage & d3d::USAGE_DEPTHSTENCIL)
		need |= VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
	else if (usage & d3d::USAGE_RENDERTARGET)
		need |= VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
	else
		need |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
	return (f->features & need) == need;
}

uint32_t FormatTable::rowBytes(uint32_t d3dFormat, uint32_t width) const
{
	const FormatInfo* f = get(d3dFormat);
	return f ? (width + f->blockW - 1) / f->blockW * f->blockBytes : 0;
}

uint32_t FormatTable::rows(uint32_t d3dFormat, uint32_t height) const
{
	const FormatInfo* f = get(d3dFormat);
	return f ? (height + f->blockH - 1) / f->blockH : 0;
}

uint64_t FormatTable::levelBytes(uint32_t d3dFormat, uint32_t width, uint32_t height) const
{
	return uint64_t(rowBytes(d3dFormat, width)) * rows(d3dFormat, height);
}

}} // namespace GXRender::Res
