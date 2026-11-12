/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2026 GeneralsX contributors
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
**
**	This program is distributed in the hope that it will be useful,
**	but WITHOUT ANY WARRANTY; without even the implied warranty of
**	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**	GNU General Public License for more details.
**
**	You should have received a copy of the GNU General Public License
**	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

// GXRES: D3D8 format -> Vulkan format, per device.
//
// The mapping is DXVK's (references/fbraz3-dxvk src/d3d9/d3d9_format.cpp ConvertFormatUnfixed + the depth fix-ups in the
// format table constructor), restricted to the formats D3D8 has, so the native path samples the same texels and depth-tests
// at the same precision as the CLASSIC path on the same device. Formats DXVK only reaches through a conversion shader
// (L6V5U5, X8L8V8U8, W11V11U10) or not at all (R8G8B8, P8, A8P8, R3G3B2, A8R3G3B2, D32, D15S1, D24X4S4) are unsupported here:
// the game never creates one, and supports() answers "no" exactly where DXVK's CheckDeviceFormat does.
#pragma once
#include "VkCommon.h"
#include <cstdint>
#include <vector>

namespace GXRender { namespace Res {

struct FormatInfo
{
	uint32_t d3d = 0;
	VkFormat vk = VK_FORMAT_UNDEFINED;     // what images of this format are created with on THIS device
	VkComponentMapping swizzle = {};       // sampled views; attachments use identity
	uint8_t blockW = 1, blockH = 1;        // 4x4 for BC
	uint16_t blockBytes = 0;               // bytes per texel, or per 4x4 block
	VkImageAspectFlags aspect = 0;
	bool depth = false, stencil = false;
	VkFormatFeatureFlags features = 0;     // optimal-tiling features the device reports for vk
	const char* name = "";
};

// Capability overrides that FORCE the paths a stronger GPU never takes (on the M3 the unsupported-format and
// reduced-capability paths are forced in tests, so its hardware cannot hide a fallback defect). Defaults = the device's own.
struct FormatCaps
{
	bool noBC = false;        // BC1-3 (DXT1-5) and BC7 unsampled: DXT art decodes to A8R8G8B8, HD art cannot take BC7
	bool noASTC = false;      // ASTC unsampled: HD art takes BC7
	uint32_t maxSamples = 0;  // 0 = the device's; else MSAA requests also clamp to this
	uint32_t maxDim = 0;      // 0 = the device's maxImageDimension2D; else larger textures are refused below it
	uint32_t maxSampledSamples = 0; // 0 = the device's sampledImageColorSampleCounts; else a SAMPLED MSAA colour target above
	                                // this count is refused (RenderTargetDesc::sampled)
	bool rgba16f = false;     // register GX_RGBA16F (the motion bridge's O, A35): only when GX_MOTION_UNITS=1, so key off
	                          // probes and knows no new format
};

class FormatTable
{
public:
	// Queries every mapped format once. D24S8/D24X8 fall back to D32_SFLOAT_S8_UINT/D32_SFLOAT when the device cannot both
	// sample and depth-attach D24_UNORM_S8_UINT (Apple GPUs), as DXVK does. caps can only take capabilities away.
	void init(VkPhysicalDevice physical, const FormatCaps& caps = {});
	const FormatCaps& caps() const { return m_caps; }
	uint32_t maxDim() const { return m_maxDim; }   // the largest texture width/height accepted (caps applied)
	// A DXTn texture this device cannot sample: GxTexture keeps the blocks on the host and uploads them decoded (GxDecode.h).
	bool decodes(uint32_t d3dFormat) const;
	// What an image of this format is created as: A8R8G8B8 when decoded, else the format itself (nullptr: unmapped).
	const FormatInfo* imageFormat(uint32_t d3dFormat) const;
	// HD art: d3d::GX_ASTC_4x4 if sampled-capable, else d3d::GX_BC7 if sampled-capable, else 0 - no device the plan supports
	// has neither, so there is no CPU decoder for them and such a texture is refused, never silently swapped.
	uint32_t chooseHdFormat() const;
	// nullptr: not a D3D8 format this table maps.
	const FormatInfo* get(uint32_t d3dFormat) const;
	// The CheckDeviceFormat answer for a resource of this usage (d3d::USAGE_* bits; 0 = a plain sampled texture).
	bool supports(uint32_t d3dFormat, uint32_t usage) const;
	// Tight bytes of one level, rows rounded up to whole blocks.
	uint64_t levelBytes(uint32_t d3dFormat, uint32_t width, uint32_t height) const;
	uint32_t rowBytes(uint32_t d3dFormat, uint32_t width) const;
	uint32_t rows(uint32_t d3dFormat, uint32_t height) const;
	const std::vector<FormatInfo>& all() const { return m_formats; }
	bool depthFellBack() const { return m_depthFallback; }

private:
	std::vector<FormatInfo> m_formats;
	FormatCaps m_caps;
	uint32_t m_maxDim = 0;
	bool m_depthFallback = false;
};

}} // namespace GXRender::Res
