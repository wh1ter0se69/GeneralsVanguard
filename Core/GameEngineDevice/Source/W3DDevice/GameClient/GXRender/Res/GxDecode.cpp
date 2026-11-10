// GeneralsX: WS-C week 2: BC1/BC2/BC3 CPU decode (see GxDecode.h). Bit layouts: Khronos Data
// Format Specification "S3TC Compressed Texture Image Formats"; colour rounding as the D3D reference ((2a+b+1)/3) - a GPU may
// round the interpolants differently, which is why the tests compare against the GPU's own decode with a tolerance.
#include "GxDecode.h"
#include "GxD3D.h"

namespace GXRender { namespace Res {

namespace {

uint16_t rd16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }

void rgb565(uint16_t c, int out[3])
{
	const int r = c >> 11 & 31, g = c >> 5 & 63, b = c & 31;
	out[0] = r << 3 | r >> 2;
	out[1] = g << 2 | g >> 4;
	out[2] = b << 3 | b >> 2;
}

// The colour half of every BC1-3 block. threeColour: BC1's c0 <= c1 mode (third colour = midpoint, fourth = transparent
// black); BC2/BC3 always decode four colours.
void colourBlock(const uint8_t* b, bool bc1, bool defect, uint8_t out[64])
{
	const uint16_t c0 = rd16(b), c1 = rd16(b + 2);
	int p[4][4];
	rgb565(c0, p[0]);
	rgb565(c1, p[1]);
	p[0][3] = p[1][3] = p[2][3] = p[3][3] = 255;
	const bool four = !bc1 || c0 > c1;
	for (int ch = 0; ch < 3; ++ch) {
		if (four) {
			p[2][ch] = (2 * p[0][ch] + p[1][ch] + 1) / 3;
			p[3][ch] = (p[0][ch] + 2 * p[1][ch] + 1) / 3;
		} else {
			p[2][ch] = (p[0][ch] + p[1][ch]) / 2;
			p[3][ch] = 0;
		}
	}
	if (!four)
		p[3][3] = 0;
	if (defect)
		for (int ch = 0; ch < 4; ++ch) {
			const int t = p[2][ch];
			p[2][ch] = p[3][ch];
			p[3][ch] = t;
		}
	const uint32_t idx = uint32_t(b[4]) | uint32_t(b[5]) << 8 | uint32_t(b[6]) << 16 | uint32_t(b[7]) << 24;
	for (int i = 0; i < 16; ++i) {
		const int* c = p[idx >> (2 * i) & 3];
		out[4 * i + 0] = uint8_t(c[2]); // B
		out[4 * i + 1] = uint8_t(c[1]); // G
		out[4 * i + 2] = uint8_t(c[0]); // R
		out[4 * i + 3] = uint8_t(c[3]); // A
	}
}

} // namespace

bool canDecode(uint32_t f)
{
	return f == d3d::DXT1 || f == d3d::DXT2 || f == d3d::DXT3 || f == d3d::DXT4 || f == d3d::DXT5;
}

void decodeBlockBGRA(uint32_t f, const uint8_t* b, uint8_t out[64], bool defect)
{
	if (f == d3d::DXT1) {
		colourBlock(b, true, defect, out);
		return;
	}
	colourBlock(b + 8, false, defect, out);
	if (f == d3d::DXT2 || f == d3d::DXT3) {
		for (int i = 0; i < 16; ++i)
			out[4 * i + 3] = uint8_t((b[i / 2] >> (4 * (i & 1)) & 15) * 17);
		return;
	}
	// DXT4/DXT5: two 8-bit endpoints, 3-bit indices
	const int a0 = b[0], a1 = b[1];
	int a[8] = {a0, a1};
	if (a0 > a1)
		for (int i = 1; i < 7; ++i)
			a[i + 1] = ((7 - i) * a0 + i * a1 + 3) / 7;
	else {
		for (int i = 1; i < 5; ++i)
			a[i + 1] = ((5 - i) * a0 + i * a1 + 2) / 5;
		a[6] = 0;
		a[7] = 255;
	}
	uint64_t bits = 0;
	for (int i = 0; i < 6; ++i)
		bits |= uint64_t(b[2 + i]) << (8 * i);
	for (int i = 0; i < 16; ++i)
		out[4 * i + 3] = uint8_t(a[bits >> (3 * i) & 7]);
}

}} // namespace GXRender::Res
