#pragma once

// GeneralsX @bugfix C · mip-box-filter 18/10/2026 One 2:1 box reduction for every platform.
//
// D3DXLoadSurfaceFromSurface builds each mip level from the one above it (D3DXFilterTexture: the terrain atlas, its
// blend and edge textures, the tree atlas, surface-built textures). Apple averaged every 2x2 block; Windows and Linux
// called gli::generate_mipmaps(FILTER_LINEAR), which for a 2:1 step samples the source at i * (W - 1) / (W/2 - 1):
// the left and top half of every level is the even texel POINT-SAMPLED, the rest a blend whose weight drifts with the
// position. That aliases (distant ground and trees shimmer) and a tile's mips depend on where it sits in an atlas.
//
// This is the Apple box, byte for byte, for every format it handled (8-bit channels and A1R5G5B5's 5-bit channels:
// (sum + 2) >> 2; a 1-bit channel: set when 2+ of the 4 are set). It also reduces a level that is 1 texel wide or
// high (a non-square texture's tail: 4x1 -> 2x1) by repeating the single row or column, so 2 texels are averaged,
// and the other 16-bit layouts with the same per-channel rule. No D3D types here, so it can be tested offline.

#include <cstdint>

namespace d3dx8box
{

enum Layout
{
	LAYOUT_UNSUPPORTED,
	LAYOUT_8888,		// A8R8G8B8, X8R8G8B8: four 8-bit channels
	LAYOUT_1555,		// A1R5G5B5, X1R5G5B5
	LAYOUT_565,			// R5G6B5
	LAYOUT_4444,		// A4R4G4B4, X4R4G4B4
};

struct Channel
{
	uint32_t shift;
	uint32_t bits;
};

template <typename Pixel, int ChannelCount>
inline void reduce(const uint8_t *src, uint32_t srcPitch, uint32_t srcWidth, uint32_t srcHeight,
	uint8_t *dst, uint32_t dstPitch, uint32_t dstWidth, uint32_t dstHeight, const Channel (&channels)[ChannelCount])
{
	const uint32_t xStep = srcWidth > 1 ? 1 : 0;
	const uint32_t yStep = srcHeight > 1 ? 1 : 0;
	for (uint32_t y = 0; y < dstHeight; y++)
	{
		const Pixel *row0 = (const Pixel *)(src + (y * 2) * srcPitch);
		const Pixel *row1 = (const Pixel *)(src + (y * 2 + yStep) * srcPitch);
		Pixel *out = (Pixel *)(dst + y * dstPitch);
		for (uint32_t x = 0; x < dstWidth; x++)
		{
			const uint32_t sx0 = x * 2;
			const uint32_t sx1 = sx0 + xStep;
			const uint32_t p00 = row0[sx0];
			const uint32_t p10 = row0[sx1];
			const uint32_t p01 = row1[sx0];
			const uint32_t p11 = row1[sx1];
			uint32_t result = 0;
			for (int c = 0; c < ChannelCount; c++)
			{
				const uint32_t shift = channels[c].shift;
				const uint32_t mask = (1u << channels[c].bits) - 1;
				const uint32_t sum = ((p00 >> shift) & mask) + ((p10 >> shift) & mask) +
				                     ((p01 >> shift) & mask) + ((p11 >> shift) & mask);
				const uint32_t value = channels[c].bits == 1 ? (sum >= 2 ? 1u : 0u) : ((sum + 2) >> 2);
				result |= value << shift;
			}
			out[x] = (Pixel)result;
		}
	}
}

// False when the layout is unknown or dst is not the next mip level of src (max(1, n / 2) on each axis).
inline bool reduceLevel(Layout layout, const void *src, uint32_t srcPitch, uint32_t srcWidth, uint32_t srcHeight,
	void *dst, uint32_t dstPitch, uint32_t dstWidth, uint32_t dstHeight)
{
	const uint32_t halfWidth = srcWidth > 1 ? srcWidth / 2 : 1;
	const uint32_t halfHeight = srcHeight > 1 ? srcHeight / 2 : 1;
	if (srcWidth == 0 || srcHeight == 0 || dstWidth != halfWidth || dstHeight != halfHeight)
	{
		return false;
	}

	const uint8_t *s = (const uint8_t *)src;
	uint8_t *d = (uint8_t *)dst;
	switch (layout)
	{
		case LAYOUT_8888:
		{
			static const Channel c[4] = { {24, 8}, {16, 8}, {8, 8}, {0, 8} };
			reduce<uint32_t>(s, srcPitch, srcWidth, srcHeight, d, dstPitch, dstWidth, dstHeight, c);
			return true;
		}
		case LAYOUT_1555:
		{
			static const Channel c[4] = { {15, 1}, {10, 5}, {5, 5}, {0, 5} };
			reduce<uint16_t>(s, srcPitch, srcWidth, srcHeight, d, dstPitch, dstWidth, dstHeight, c);
			return true;
		}
		case LAYOUT_565:
		{
			static const Channel c[3] = { {11, 5}, {5, 6}, {0, 5} };
			reduce<uint16_t>(s, srcPitch, srcWidth, srcHeight, d, dstPitch, dstWidth, dstHeight, c);
			return true;
		}
		case LAYOUT_4444:
		{
			static const Channel c[4] = { {12, 4}, {8, 4}, {4, 4}, {0, 4} };
			reduce<uint16_t>(s, srcPitch, srcWidth, srcHeight, d, dstPitch, dstWidth, dstHeight, c);
			return true;
		}
		default:
			return false;
	}
}

} // namespace d3dx8box
