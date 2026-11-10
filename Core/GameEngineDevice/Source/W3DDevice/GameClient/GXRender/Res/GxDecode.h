// GeneralsX: WS-C week 2: the decode fallback.
//
// A device without BC (Apple7/8 iPads may lack it; M3 = Apple9 has it, so the tests FORCE this path there) cannot sample the
// game's DXT art. GxTexture then keeps the D3D8 contract - LockRect still hands out 4x4 blocks - and decodes each uploaded
// rect on the CPU into an A8R8G8B8 image. HD art has no CPU decoder on purpose: it ships as ASTC (iPad) or BC7 (desktop), and
// every GPU the plan supports samples one of the two (FormatTable::chooseHdFormat).
#pragma once
#include <cstdint>

namespace GXRender { namespace Res {

// DXT1..DXT5 (DXT2/DXT4 decode as DXT3/DXT5: D3D8 leaves the premultiply to the shader).
bool canDecode(uint32_t d3dFormat);

// One 4x4 block -> 16 texels, row-major, A8R8G8B8 byte order (B, G, R, A). defect = the planted fail-arm error: the two
// interpolated colours swap places (only a pixel comparison with the GPU's own BC decode can see it).
void decodeBlockBGRA(uint32_t d3dFormat, const uint8_t* block, uint8_t out[64], bool defect = false);

}} // namespace GXRender::Res
