/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 Electronic Arts Inc.
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

// GeneralsX: WS-G Task 5: gxl2m::shaderApply, a line-faithful port of ShaderClass::Apply
// (ZW/shader.cpp:409-1062; ZW = GeneralsMD/Code/Libraries/Source/WWVegas/WW3D2/). Engine-free: the bit layout (ZW/shader.h:48-228)
// and the D3D8 numbers are copied below. Set_DX8_Render_State -> sendRS, Set_DX8_Texture_Stage_State -> sendTSS, ShaderClass::
// CurrentShader / ShaderDirty -> State::curShader / shaderDirty, every global and caps read -> State::sg. Debug-only lines
// (SNAPSHOT_SAY, WWDEBUG_SAY, the #if 0 coloring block at :967-974) have no device effect and are left out.

#include "W3DDevice/GameClient/GXRender/Scene/GxL2Model.h"

namespace {

// D3D8 numbers, each checked against references/fbraz3-dxvk/include/native/directx/d3d8types.h (T:) or d3d8caps.h (C:).
enum : uint32_t {
	D3DRS_ZWRITEENABLE = 14, D3DRS_ALPHATESTENABLE = 15, D3DRS_SRCBLEND = 19, D3DRS_DESTBLEND = 20, D3DRS_CULLMODE = 22,
	D3DRS_ZFUNC = 23, D3DRS_ALPHAREF = 24, D3DRS_ALPHAFUNC = 25, D3DRS_ALPHABLENDENABLE = 27, D3DRS_FOGENABLE = 28,
	D3DRS_SPECULARENABLE = 29, D3DRS_FOGCOLOR = 34, D3DRS_PATCHSEGMENTS = 164,				// T:733-796
	D3DBLEND_ZERO = 1, D3DBLEND_ONE = 2, D3DBLEND_SRCCOLOR = 3, D3DBLEND_INVSRCCOLOR = 4, D3DBLEND_SRCALPHA = 5,
	D3DBLEND_INVSRCALPHA = 6, D3DBLEND_DESTCOLOR = 9,										// T:509-517
	D3DCMP_LESSEQUAL = 4, D3DCMP_GREATEREQUAL = 7,											// T:540, :543
	D3DCULL_NONE = 1,																		// T:561
	D3DTSS_COLOROP = 1, D3DTSS_COLORARG1 = 2, D3DTSS_COLORARG2 = 3, D3DTSS_ALPHAOP = 4, D3DTSS_ALPHAARG1 = 5,
	D3DTSS_ALPHAARG2 = 6, D3DTSS_TEXCOORDINDEX = 11,										// T:912-922
	D3DTSS_TCI_PASSTHRU = 0,																// T:79
	D3DTA_DIFFUSE = 0, D3DTA_CURRENT = 1, D3DTA_TEXTURE = 2,								// T:139-141
	D3DTOP_DISABLE = 1, D3DTOP_SELECTARG1 = 2, D3DTOP_SELECTARG2 = 3, D3DTOP_MODULATE = 4, D3DTOP_MODULATE2X = 5,
	D3DTOP_ADD = 7, D3DTOP_ADDSIGNED = 8, D3DTOP_ADDSIGNED2X = 9, D3DTOP_SUBTRACT = 10, D3DTOP_ADDSMOOTH = 11,
	D3DTOP_BLENDTEXTUREALPHA = 13, D3DTOP_BLENDCURRENTALPHA = 16, D3DTOP_MODULATEALPHA_ADDCOLOR = 18, D3DTOP_BUMPENVMAP = 22,
	D3DTOP_BUMPENVMAPLUMINANCE = 23,														// T:881-903
	D3DTEXOPCAPS_SELECTARG1 = 0x2, D3DTEXOPCAPS_MODULATE = 0x8, D3DTEXOPCAPS_MODULATE2X = 0x10, D3DTEXOPCAPS_ADD = 0x40,
	D3DTEXOPCAPS_ADDSIGNED = 0x80, D3DTEXOPCAPS_ADDSIGNED2X = 0x100, D3DTEXOPCAPS_SUBTRACT = 0x200, D3DTEXOPCAPS_ADDSMOOTH = 0x400,
	D3DTEXOPCAPS_BLENDTEXTUREALPHA = 0x1000, D3DTEXOPCAPS_BLENDCURRENTALPHA = 0x8000, D3DTEXOPCAPS_BUMPENVMAP = 0x200000,
	D3DTEXOPCAPS_BUMPENVMAPLUMINANCE = 0x400000,											// C:174-195
};

// DX8Caps ids for the voodoo3 test (Core/Libraries/Source/WWVegas/WW3D2/dx8caps.h:57-65 VendorIdType, :163-166 DeviceType3Dfx).
enum : uint32_t { VENDOR_3DFX = 7, DEVICE_3DFX_VOODOO_3 = 2 };

// ShaderClass's bit layout and field values (ZW/shader.h:48-64 shifts, :212-228 masks, :84-201 enums).
enum : uint32_t {
	SHIFT_DEPTHCOMPARE = 0, SHIFT_DEPTHMASK = 3, SHIFT_COLORMASK = 4, SHIFT_DSTBLEND = 5, SHIFT_FOG = 8, SHIFT_PRIGRADIENT = 10,
	SHIFT_SECGRADIENT = 13, SHIFT_SRCBLEND = 14, SHIFT_TEXTURING = 16, SHIFT_NPATCHENABLE = 17, SHIFT_ALPHATEST = 18,
	SHIFT_CULLMODE = 19, SHIFT_POSTDETAILCOLORFUNC = 20, SHIFT_POSTDETAILALPHAFUNC = 24,
	MASK_DEPTHCOMPARE = 7u << 0, MASK_DEPTHMASK = 1u << 3, MASK_COLORMASK = 1u << 4, MASK_DSTBLEND = 7u << 5, MASK_FOG = 3u << 8,
	MASK_PRIGRADIENT = 7u << 10, MASK_SECGRADIENT = 1u << 13, MASK_SRCBLEND = 3u << 14, MASK_TEXTURING = 1u << 16,
	MASK_NPATCHENABLE = 1u << 17, MASK_ALPHATEST = 1u << 18, MASK_CULLMODE = 1u << 19, MASK_POSTDETAILCOLORFUNC = 15u << 20,
	MASK_POSTDETAILALPHAFUNC = 7u << 24,
	ALPHATEST_ENABLE = 1, COLOR_WRITE_ENABLE = 1, TEXTURING_ENABLE = 1,
	DETAILALPHA_DISABLE = 0, DETAILALPHA_DETAIL, DETAILALPHA_SCALE, DETAILALPHA_INVSCALE,
	DETAILCOLOR_DISABLE = 0, DETAILCOLOR_DETAIL, DETAILCOLOR_SCALE, DETAILCOLOR_INVSCALE, DETAILCOLOR_ADD, DETAILCOLOR_SUB,
	DETAILCOLOR_SUBR, DETAILCOLOR_BLEND, DETAILCOLOR_DETAILBLEND, DETAILCOLOR_ADDSIGNED, DETAILCOLOR_ADDSIGNED2X,
	DETAILCOLOR_SCALE2X, DETAILCOLOR_MODALPHAADDCOLOR,
	DSTBLEND_MAX = 6,
	FOG_DISABLE = 0, FOG_ENABLE, FOG_SCALE_FRAGMENT, FOG_WHITE,
	GRADIENT_DISABLE = 0, GRADIENT_MODULATE, GRADIENT_ADD, GRADIENT_BUMPENVMAP, GRADIENT_BUMPENVMAPLUMINANCE, GRADIENT_MODULATE2X,
	SRCBLEND_MAX = 4,
};

// ZW/shader.cpp:364-394, the blend LUTs. srcBlendLUT[3] (SRCBLEND_ONE_MINUS_SRC_ALPHA) is D3DBLEND_DESTCOLOR in the original.
struct Blend { uint32_t func; bool useAlpha; };
constexpr Blend srcBlendLUT[SRCBLEND_MAX] = {
	{ D3DBLEND_ZERO, false }, { D3DBLEND_ONE, false }, { D3DBLEND_SRCALPHA, true }, { D3DBLEND_DESTCOLOR, true } };
constexpr Blend dstBlendLUT[DSTBLEND_MAX] = {
	{ D3DBLEND_ZERO, false }, { D3DBLEND_ONE, false }, { D3DBLEND_SRCCOLOR, false }, { D3DBLEND_INVSRCCOLOR, false },
	{ D3DBLEND_SRCALPHA, true }, { D3DBLEND_INVSRCALPHA, true } };

// Core/Libraries/Source/WWVegas/WW3D2/dx8wrapper.h:915-918 Gx_Truth_Raw_TSS: a raw device write (DX8CALL(SetTextureStageState))
// records the truth only - the wrapper's cache never sees it. (GxTruthRawCalls, the per-frame counter, is not modelled.)
void rawTSS(gxl2m::State &s, unsigned stage, unsigned type, uint32_t value)
{
	if (s.truthOn && stage < (unsigned)gxl2m::kStages) { s.dev.tss[stage][type] = value; s.dev.tssKnown[stage][type] = true; }
}

} // namespace

namespace gxl2m {

void shaderApply(State &s, uint32_t ShaderBits)
{
	const uint32_t capsDevice = s.sg.device;				// DX8Caps::Get_Device(), ZW/shader.cpp:553
	const bool supportModAlphaAddClr = s.sg.modAlphaAddClr;	// DX8Caps::Support_ModAlphaAddClr(), ZW/shader.cpp:847
	const unsigned npatchesLevel = s.sg.npatchesLevel;		// WW3D::Get_NPatches_Level(), ZW/shader.cpp:1053

	// ZW/shader.h:287-300, the getters. Get_Depth_Compare's original reads `ShaderBits&MASK_DEPTHCOMPARE>>SHIFT_DEPTHCOMPARE`,
	// i.e. bits & (7 >> 0): the same value, since the shift is 0.
	auto Get_Depth_Compare = [&] { return ShaderBits & (MASK_DEPTHCOMPARE >> SHIFT_DEPTHCOMPARE); };
	auto Get_Depth_Mask = [&] { return (ShaderBits & MASK_DEPTHMASK) >> SHIFT_DEPTHMASK; };
	auto Get_Color_Mask = [&] { return (ShaderBits & MASK_COLORMASK) >> SHIFT_COLORMASK; };
	auto Get_Post_Detail_Alpha_Func = [&] { return (ShaderBits & MASK_POSTDETAILALPHAFUNC) >> SHIFT_POSTDETAILALPHAFUNC; };
	auto Get_Post_Detail_Color_Func = [&] { return (ShaderBits & MASK_POSTDETAILCOLORFUNC) >> SHIFT_POSTDETAILCOLORFUNC; };
	auto Get_Alpha_Test = [&] { return (ShaderBits & MASK_ALPHATEST) >> SHIFT_ALPHATEST; };
	auto Get_Cull_Mode = [&] { return (ShaderBits & MASK_CULLMODE) >> SHIFT_CULLMODE; };
	auto Get_Dst_Blend_Func = [&] { return (ShaderBits & MASK_DSTBLEND) >> SHIFT_DSTBLEND; };
	auto Get_Fog_Func = [&] { return (ShaderBits & MASK_FOG) >> SHIFT_FOG; };
	auto Get_Primary_Gradient = [&] { return (ShaderBits & MASK_PRIGRADIENT) >> SHIFT_PRIGRADIENT; };
	auto Get_Secondary_Gradient = [&] { return (ShaderBits & MASK_SECGRADIENT) >> SHIFT_SECGRADIENT; };
	auto Get_Src_Blend_Func = [&] { return (ShaderBits & MASK_SRCBLEND) >> SHIFT_SRCBLEND; };
	auto Get_Texturing = [&] { return (ShaderBits & MASK_TEXTURING) >> SHIFT_TEXTURING; };
	auto Get_NPatch_Enable = [&] { return (ShaderBits & MASK_NPATCHENABLE) >> SHIFT_NPATCHENABLE; };

	// ZW/shader.cpp:411-428: the diff, the early return, and the cache update BEFORE any write.
	uint32_t diff;

	const uint32_t TextureOpCaps = s.sg.textureOpCaps;

	if (s.mutant == 4)
		diff = 0xffffffffu;	// M4 (CONTRACT_L2 §4.5; the check's runtime GX_L2_MUTANT): send every bit - the shader cache ignored
	else if (s.shaderDirty)
	{
		diff = 0xffffffffu;
	}
	else
	{
		diff = s.curShader ^ ShaderBits;
	}

	if (!diff) return;

	s.curShader = ShaderBits;
	s.shaderDirty = false;

	// ZW/shader.cpp:429-489 COLOR MASK, blend, alpha test
	if (diff & (MASK_COLORMASK | MASK_SRCBLEND | MASK_DSTBLEND | MASK_ALPHATEST))
	{
		uint32_t planeMask = 0xffffff;

		if (Get_Color_Mask() != COLOR_WRITE_ENABLE)
			planeMask = 0;

		uint32_t sf;
		uint32_t df;
		bool blendAlpha = false;

		if (!planeMask)
		{
			sf = D3DBLEND_ZERO;
			df = D3DBLEND_ONE;
		}
		else
		{
			// The original indexes dstBlendLUT[6..7] past its end for dst blend 6/7 (a 3-bit field, 6 entries): undefined there,
			// so the model marks the state unknown and reads entry 0 instead of reading past the table.
			uint32_t dst = Get_Dst_Blend_Func();
			if (dst >= DSTBLEND_MAX) { ++s.unknown; dst = 0; }
			sf = srcBlendLUT[ int(Get_Src_Blend_Func()) ].func;
			df = dstBlendLUT[ int(dst) ].func;
			blendAlpha |= srcBlendLUT[ int(Get_Src_Blend_Func()) ].useAlpha;
			blendAlpha |= dstBlendLUT[ int(dst) ].useAlpha;
		}

		bool blendOn = false;

		if (sf != D3DBLEND_ONE || df != D3DBLEND_ZERO)
		{
			sendRS(s, D3DRS_SRCBLEND, sf);
			sendRS(s, D3DRS_DESTBLEND, df);
			blendOn = true;
		}
#if GX_L2M_MUTANT == 6
		else
		{
			sendRS(s, D3DRS_SRCBLEND, sf);	// M6: blend off also writes SRC/DESTBLEND (the partial-write rule broken)
			sendRS(s, D3DRS_DESTBLEND, df);
		}
#endif
		sendRS(s, D3DRS_ALPHABLENDENABLE, blendOn);

		bool alphaTest = false;

		if (Get_Alpha_Test() == ALPHATEST_ENABLE)
		{
			const uint32_t alphareference = 0x60;	// Alpha reference value that produces best results with mip-mapped textures.

			// Never true in practice: neither LUT entry nor the colour-mask-off pair is INVSRCALPHA for sf. Ported as written.
			if (sf == D3DBLEND_INVSRCALPHA)
			{
				sendRS(s, D3DRS_ALPHAREF, 0xff - alphareference);
#if GX_L2M_MUTANT != 5
				sendRS(s, D3DRS_ALPHAFUNC, D3DCMP_LESSEQUAL);
#endif
			}
			else
			{
				sendRS(s, D3DRS_ALPHAREF, alphareference);
#if GX_L2M_MUTANT != 5
				sendRS(s, D3DRS_ALPHAFUNC, D3DCMP_GREATEREQUAL);	// M5 drops both ALPHAFUNC writes
#endif
			}
			blendAlpha = true;
			alphaTest = true;
		}
		(void)blendAlpha;	// computed and never read, in the original too
		sendRS(s, D3DRS_ALPHATESTENABLE, alphaTest);

		diff &= ~(MASK_COLORMASK | MASK_SRCBLEND | MASK_DSTBLEND | MASK_ALPHATEST);
		if (!diff)
			return;
	}

	// ZW/shader.cpp:491-532 fog
	if (diff & (MASK_FOG))
	{
		if (s.sg.fogAllowed && s.sg.fogEnable) {

			bool fm = false;
			uint32_t fogColor = s.sg.fogColor;

			switch (Get_Fog_Func())
			{
			case FOG_ENABLE:
				fm = true;
				break;
			case FOG_SCALE_FRAGMENT:
				fogColor = 0;
				fm = true;
				break;
			case FOG_WHITE:
				fogColor = 0xffffff;
				fm = true;
				break;
			case FOG_DISABLE:
				fm = false;
				break;
			}

			sendRS(s, D3DRS_FOGENABLE, fm);

			if (fm)
			{
				sendRS(s, D3DRS_FOGCOLOR, fogColor);
			}

		} else {
			sendRS(s, D3DRS_FOGENABLE, false);
		}

		diff &= ~(MASK_FOG);
		if (!diff)
			return;
	}

	// ZW/shader.cpp:534-550 Defaults
	uint32_t PricOp = D3DTOP_SELECTARG1;
	uint32_t PricArg1 = D3DTA_DIFFUSE;
	uint32_t PricArg2 = D3DTA_DIFFUSE;

	uint32_t PriaOp = D3DTOP_SELECTARG1;
	uint32_t PriaArg1 = D3DTA_DIFFUSE;
	uint32_t PriaArg2 = D3DTA_DIFFUSE;

	uint32_t SeccOp = D3DTOP_DISABLE;
	uint32_t SeccArg1 = D3DTA_TEXTURE;
	uint32_t SeccArg2 = D3DTA_CURRENT;

	uint32_t SecaOp = D3DTOP_DISABLE;
	uint32_t SecaArg1 = D3DTA_TEXTURE;
	uint32_t SecaArg2 = D3DTA_CURRENT;

	// ZW/shader.cpp:552-562 the voodoo3 test and the stage masks
	bool voodoo3 = (s.sg.vendor == VENDOR_3DFX) &&
				   (capsDevice == DEVICE_3DFX_VOODOO_3);
	uint32_t pri_mask = MASK_PRIGRADIENT | MASK_TEXTURING;
	uint32_t sec_mask = MASK_POSTDETAILALPHAFUNC | MASK_POSTDETAILCOLORFUNC | MASK_TEXTURING;

	// Voodoo3s need to keep track of any changes in any of the above
	// because it shuffles the stages around
	if (voodoo3) {
		pri_mask |= sec_mask;
		sec_mask = pri_mask;
	}

	// ZW/shader.cpp:564-687 stage 0 colour / alpha op
	if (diff & pri_mask)
	{
		if (Get_Texturing() == TEXTURING_ENABLE)
		{
			switch (Get_Primary_Gradient())
			{
			case GRADIENT_DISABLE:
				//Decal
				PricOp = D3DTOP_SELECTARG1;
				PricArg1 = D3DTA_TEXTURE;
				PricArg2 = D3DTA_CURRENT;
				PriaOp = D3DTOP_SELECTARG1;
				PriaArg1 = D3DTA_TEXTURE;
				PriaArg2 = D3DTA_CURRENT;
				break;
			default:
			case GRADIENT_MODULATE:
				PricOp = D3DTOP_MODULATE;
				PricArg1 = D3DTA_TEXTURE;
				PricArg2 = D3DTA_DIFFUSE;
				PriaOp = D3DTOP_MODULATE;
				PriaArg1 = D3DTA_TEXTURE;
				PriaArg2 = D3DTA_DIFFUSE;
				break;
			case GRADIENT_ADD:
				//Modulate Alpha
				if (!(TextureOpCaps & D3DTEXOPCAPS_ADD))
					PricOp = D3DTOP_MODULATE;
				else
					PricOp = D3DTOP_ADD;
				PricArg1 = D3DTA_TEXTURE;
				PricArg2 = D3DTA_DIFFUSE;
				PriaOp = D3DTOP_MODULATE;
				PriaArg1 = D3DTA_TEXTURE;
				PriaArg2 = D3DTA_DIFFUSE;
				break;

			// Bump map is a hack currently as we only have two stages in use!
			case GRADIENT_BUMPENVMAP:
				if (TextureOpCaps & D3DTEXOPCAPS_BUMPENVMAP)
				{
					PricOp = D3DTOP_BUMPENVMAP;
					PricArg1 = D3DTA_TEXTURE;
					PricArg2 = D3DTA_DIFFUSE;
					PriaOp = D3DTOP_DISABLE;
					PriaArg1 = D3DTA_TEXTURE;
					PriaArg2 = D3DTA_CURRENT;
				} else {
					PricOp = D3DTOP_SELECTARG1;
					PricArg1 = D3DTA_DIFFUSE;
					PricArg2 = D3DTA_DIFFUSE;
					PriaOp = D3DTOP_SELECTARG1;
					PriaArg1 = D3DTA_DIFFUSE;
					PriaArg2 = D3DTA_DIFFUSE;
				}
				break;

			// Bump map is a hack currently as we only have two stages in use!
			case GRADIENT_BUMPENVMAPLUMINANCE:
				if (TextureOpCaps & D3DTEXOPCAPS_BUMPENVMAPLUMINANCE)
				{
					PricOp = D3DTOP_BUMPENVMAPLUMINANCE;
					PricArg1 = D3DTA_TEXTURE;
					PricArg2 = D3DTA_DIFFUSE;
					PriaOp = D3DTOP_DISABLE;
					PriaArg1 = D3DTA_TEXTURE;
					PriaArg2 = D3DTA_CURRENT;
				} else {
					PricOp = D3DTOP_SELECTARG1;
					PricArg1 = D3DTA_DIFFUSE;
					PricArg2 = D3DTA_DIFFUSE;
					PriaOp = D3DTOP_SELECTARG1;
					PriaArg1 = D3DTA_DIFFUSE;
					PriaArg2 = D3DTA_DIFFUSE;
				}
				break;

			case GRADIENT_MODULATE2X:
				//Modulate Alpha
				// The original tests the OP value D3DTOP_MODULATE2X (5 = the SELECTARG1|DISABLE caps bits), not
				// D3DTEXOPCAPS_MODULATE2X. Ported as written.
				if (!(TextureOpCaps & D3DTOP_MODULATE2X))
					PricOp = D3DTOP_MODULATE;
				else
					PricOp = D3DTOP_MODULATE2X;
				PricArg1 = D3DTA_TEXTURE;
				PricArg2 = D3DTA_DIFFUSE;
				PriaOp = D3DTOP_MODULATE;
				PriaArg1 = D3DTA_TEXTURE;
				PriaArg2 = D3DTA_DIFFUSE;
				break;
			}

		}
		else
		{
			switch (Get_Primary_Gradient())
			{
			case GRADIENT_DISABLE:
				PricOp = D3DTOP_DISABLE;
				PricArg1 = D3DTA_TEXTURE;
				PricArg2 = D3DTA_CURRENT;
				PriaOp = D3DTOP_DISABLE;
				PriaArg1 = D3DTA_TEXTURE;
				PriaArg2 = D3DTA_CURRENT;
				break;
			default:
			case GRADIENT_MODULATE:
				PricOp = D3DTOP_SELECTARG2;
				PricArg1 = D3DTA_TEXTURE;
				PricArg2 = D3DTA_DIFFUSE;
				PriaOp = D3DTOP_SELECTARG2;
				PriaArg1 = D3DTA_TEXTURE;
				PriaArg2 = D3DTA_DIFFUSE;
				break;
			case GRADIENT_ADD:
				PricOp = D3DTOP_SELECTARG2;
				PricArg1 = D3DTA_TEXTURE;
				PricArg2 = D3DTA_DIFFUSE;
				PriaOp = D3DTOP_SELECTARG2;
				PriaArg1 = D3DTA_TEXTURE;
				PriaArg2 = D3DTA_DIFFUSE;
				break;
			}
		}
	}

	// ZW/shader.cpp:689-913 stage 1 colour / alpha op (post-detail)
	if (diff & sec_mask)
	{
		if (Get_Texturing() == TEXTURING_ENABLE)
		{
			switch (Get_Post_Detail_Color_Func())
			{
			default:
			case DETAILCOLOR_DISABLE:
				break;

			case DETAILCOLOR_DETAIL:
				if (TextureOpCaps & D3DTEXOPCAPS_SELECTARG1)
				{
					SeccOp = D3DTOP_SELECTARG1;
					SeccArg1 = D3DTA_TEXTURE;
					SeccArg2 = D3DTA_CURRENT;
				}
				break;

			case DETAILCOLOR_SCALE:
				if (TextureOpCaps & D3DTEXOPCAPS_MODULATE)
				{
					SeccOp = D3DTOP_MODULATE;
					SeccArg1 = D3DTA_TEXTURE;
					SeccArg2 = D3DTA_CURRENT;
				}
				break;

			case DETAILCOLOR_INVSCALE:
				if (TextureOpCaps & D3DTEXOPCAPS_ADDSMOOTH)
				{
					SeccOp = D3DTOP_ADDSMOOTH;
					SeccArg1 = D3DTA_TEXTURE;
					SeccArg2 = D3DTA_CURRENT;
				} else if (TextureOpCaps & D3DTEXOPCAPS_ADD) {
					SeccOp = D3DTOP_ADD;
					SeccArg1 = D3DTA_TEXTURE;
					SeccArg2 = D3DTA_CURRENT;
				}
				break;

			case DETAILCOLOR_ADD:
				if (TextureOpCaps & D3DTEXOPCAPS_ADD)
				{
					SeccOp = D3DTOP_ADD;
					SeccArg1 = D3DTA_TEXTURE;
					SeccArg2 = D3DTA_CURRENT;
				}
				break;

			case DETAILCOLOR_SUB:
				if (TextureOpCaps & D3DTEXOPCAPS_SUBTRACT)
				{
					SeccOp = D3DTOP_SUBTRACT;
					SeccArg1 = D3DTA_TEXTURE;
					SeccArg2 = D3DTA_CURRENT;
				}
				break;

			case DETAILCOLOR_SUBR:
				if (TextureOpCaps & D3DTEXOPCAPS_SUBTRACT)
				{
					SeccOp = D3DTOP_SUBTRACT;
					SeccArg1 = D3DTA_CURRENT;
					SeccArg2 = D3DTA_TEXTURE;
				}
				break;

			case DETAILCOLOR_BLEND:
				if (TextureOpCaps & D3DTEXOPCAPS_BLENDTEXTUREALPHA)
				{
					SeccOp = D3DTOP_BLENDTEXTUREALPHA;
					SeccArg1 = D3DTA_TEXTURE;
					SeccArg2 = D3DTA_CURRENT;
				}
				break;

			case DETAILCOLOR_DETAILBLEND:
				if (TextureOpCaps & D3DTEXOPCAPS_BLENDCURRENTALPHA)
				{
					SeccOp = D3DTOP_BLENDCURRENTALPHA;
					SeccArg1 = D3DTA_TEXTURE;
					SeccArg2 = D3DTA_CURRENT;
				}
				break;

			case DETAILCOLOR_ADDSIGNED:
				if (TextureOpCaps & D3DTEXOPCAPS_ADDSIGNED) {
					SeccOp = D3DTOP_ADDSIGNED;
					SeccArg1 = D3DTA_TEXTURE;
					SeccArg2 = D3DTA_CURRENT;
				} else if (TextureOpCaps & D3DTEXOPCAPS_ADD) {
					SeccOp = D3DTOP_ADD;
					SeccArg1 = D3DTA_TEXTURE;
					SeccArg2 = D3DTA_CURRENT;
				}
				break;

			case DETAILCOLOR_ADDSIGNED2X:
				if (TextureOpCaps & D3DTEXOPCAPS_ADDSIGNED2X) {
					SeccOp = D3DTOP_ADDSIGNED2X;
					SeccArg1 = D3DTA_TEXTURE;
					SeccArg2 = D3DTA_CURRENT;
				} else if (TextureOpCaps & D3DTEXOPCAPS_ADDSIGNED) {
					SeccOp = D3DTOP_ADDSIGNED;
					SeccArg1 = D3DTA_TEXTURE;
					SeccArg2 = D3DTA_CURRENT;
				} else if (TextureOpCaps & D3DTEXOPCAPS_ADD) {
					SeccOp = D3DTOP_ADD;
					SeccArg1 = D3DTA_TEXTURE;
					SeccArg2 = D3DTA_CURRENT;
				}
				break;

			case DETAILCOLOR_SCALE2X:
				if (TextureOpCaps & D3DTEXOPCAPS_MODULATE2X) {
					SeccOp = D3DTOP_MODULATE2X;
					SeccArg1 = D3DTA_TEXTURE;
					SeccArg2 = D3DTA_CURRENT;
				} else if (TextureOpCaps & D3DTEXOPCAPS_MODULATE) {
					SeccOp = D3DTOP_MODULATE;
					SeccArg1 = D3DTA_TEXTURE;
					SeccArg2 = D3DTA_CURRENT;
				}
				break;

			case DETAILCOLOR_MODALPHAADDCOLOR:
				if (supportModAlphaAddClr) {
					SeccOp = D3DTOP_MODULATEALPHA_ADDCOLOR;
					SeccArg1 = D3DTA_CURRENT;
					SeccArg2 = D3DTA_TEXTURE;
				} else if (TextureOpCaps & D3DTEXOPCAPS_ADD) {
					SeccOp = D3DTOP_ADD;
					SeccArg1 = D3DTA_TEXTURE;
					SeccArg2 = D3DTA_CURRENT;
				}
				break;
			}

			switch (Get_Post_Detail_Alpha_Func())
			{
			default:
			case DETAILALPHA_DISABLE:
				break;

			case DETAILALPHA_DETAIL:
				if (TextureOpCaps & D3DTEXOPCAPS_SELECTARG1)
				{
					SecaOp = D3DTOP_SELECTARG1;
					SecaArg1 = D3DTA_TEXTURE;
					SecaArg2 = D3DTA_CURRENT;
				}
				break;

			case DETAILALPHA_SCALE:
				if (TextureOpCaps & D3DTEXOPCAPS_MODULATE)
				{
					SecaOp = D3DTOP_MODULATE;
					SecaArg1 = D3DTA_TEXTURE;
					SecaArg2 = D3DTA_CURRENT;
				}
				break;

			case DETAILALPHA_INVSCALE:
				if (TextureOpCaps & D3DTEXOPCAPS_ADDSMOOTH)
				{
					SecaOp = D3DTOP_ADDSMOOTH;
					SecaArg1 = D3DTA_TEXTURE;
					SecaArg2 = D3DTA_CURRENT;
				}
				break;
			}

			// if color is enabled and alpha is disabled set to pass alpha through
			if ((SeccOp != D3DTOP_DISABLE) && (SecaOp == D3DTOP_DISABLE)) {
				SecaOp = D3DTOP_SELECTARG2;
				SecaArg2 = D3DTA_CURRENT;
			} else if ((SeccOp == D3DTOP_DISABLE) && (SecaOp != D3DTOP_DISABLE)) {
				SeccOp = D3DTOP_SELECTARG2;
				SeccArg2 = D3DTA_CURRENT;
			}
		}
	}

	bool kill_stage_2 = false;

	// ZW/shader.cpp:917-984 Apply the stage settings: stage 0 (and the voodoo3 stage-2 shuffle)
	if (diff & pri_mask) {
		// for voodoo3 supported blend modes, the stage 0 color and alpha are both diffuse
		// or both not, so we can check for color diffuse only
		if ( voodoo3 && (PricArg2 == D3DTA_DIFFUSE) &&
			  ( (SecaOp != D3DTOP_DISABLE) || (SeccOp != D3DTOP_DISABLE) )
			) {
			// Special Voodoo3 code
			// If stage 0 has a diffuse input
			// and stage 1 has an input put the diffuse in stage 2

			uint32_t tex_arg = D3DTA_CURRENT;
			if (Get_Texturing() == TEXTURING_ENABLE) {
				tex_arg = D3DTA_TEXTURE;
			}

			// this is for the bad case of using
			// stage 0 for diffuse only
			if ((PricOp == D3DTOP_SELECTARG1) && (PricArg1 == D3DTA_DIFFUSE)) {
				// set stage 0 to disable
				sendTSS(s, 0, D3DTSS_COLOROP, D3DTOP_DISABLE);
				sendTSS(s, 0, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
				// set stage 1 to accept diffuse
				if (SeccArg2 == D3DTA_CURRENT) SeccArg2 = D3DTA_DIFFUSE;
				if (SecaArg2 == D3DTA_CURRENT) SecaArg2 = D3DTA_DIFFUSE;
				// and nuke stage 2
				kill_stage_2 = true;
			} else {
				// set stage 0 to pass through what it needs
				sendTSS(s, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
				sendTSS(s, 0, D3DTSS_COLORARG1, tex_arg);
				sendTSS(s, 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
				sendTSS(s, 0, D3DTSS_ALPHAARG1, tex_arg);

				// set stage 2 to do the diffuse op
				// bypass the wrapper since it only supports 2 texture stages (raw: truth only, ZW/shader.cpp:954-961)
				rawTSS(s, 2, D3DTSS_COLOROP, PricOp);
				rawTSS(s, 2, D3DTSS_COLORARG1, D3DTA_CURRENT);
				rawTSS(s, 2, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
				rawTSS(s, 2, D3DTSS_ALPHAOP, PriaOp);
				rawTSS(s, 2, D3DTSS_ALPHAARG1, D3DTA_CURRENT);
				rawTSS(s, 2, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
				rawTSS(s, 2, D3DTSS_TEXCOORDINDEX, D3DTSS_TCI_PASSTHRU);
				++s.unknown;	// DX8CALL(SetTexture(2,nullptr)): raw, Textures[2] keeps its value; dev.tex is both, so unmodelled
				kill_stage_2 = false;
				s.shaderDirty = true;
			}
		} else {
			sendTSS(s, 0, D3DTSS_COLOROP, PricOp);
			sendTSS(s, 0, D3DTSS_COLORARG1, PricArg1);
			sendTSS(s, 0, D3DTSS_COLORARG2, PricArg2);
			sendTSS(s, 0, D3DTSS_ALPHAOP, PriaOp);
			sendTSS(s, 0, D3DTSS_ALPHAARG1, PriaArg1);
			sendTSS(s, 0, D3DTSS_ALPHAARG2, PriaArg2);
			kill_stage_2 = true;
		}
		diff &= ~(MASK_PRIGRADIENT);
	}

	// ZW/shader.cpp:986-996 stage 1
	if (diff & sec_mask) {
		sendTSS(s, 1, D3DTSS_COLOROP, SeccOp);
		sendTSS(s, 1, D3DTSS_COLORARG1, SeccArg1);
		sendTSS(s, 1, D3DTSS_COLORARG2, SeccArg2);
		sendTSS(s, 1, D3DTSS_ALPHAOP, SecaOp);
		sendTSS(s, 1, D3DTSS_ALPHAARG1, SecaArg1);
		sendTSS(s, 1, D3DTSS_ALPHAARG2, SecaArg2);
		diff &= ~(MASK_POSTDETAILCOLORFUNC);
		diff &= ~(MASK_POSTDETAILALPHAFUNC);
		diff &= ~(MASK_TEXTURING);
	}

	// ZW/shader.cpp:998-1013 Make sure to disable stage 2 for voodoos since we don't have state tracking for
	// stage 2 - bypass the wrapper since it only supports 2 texture stages (raw: truth only)
	if (voodoo3 && kill_stage_2) {
		if ((SeccOp != D3DTOP_DISABLE) && (SecaOp != D3DTOP_DISABLE)) {
			rawTSS(s, 2, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
			rawTSS(s, 2, D3DTSS_COLORARG1, D3DTA_CURRENT);
			rawTSS(s, 2, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
			rawTSS(s, 2, D3DTSS_ALPHAARG1, D3DTA_CURRENT);
		} else {
			rawTSS(s, 2, D3DTSS_COLOROP, D3DTOP_DISABLE);
			rawTSS(s, 2, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
		}
		rawTSS(s, 2, D3DTSS_TEXCOORDINDEX, D3DTSS_TCI_PASSTHRU);
		++s.unknown;	// DX8CALL(SetTexture(2,nullptr)), as above
	}

	// ZW/shader.cpp:1015-1058 the final block: specular, depth compare, depth mask, cull, npatches, alpha test
	if (!diff)
		return;

	sendRS(s, D3DRS_SPECULARENABLE, Get_Secondary_Gradient());

	// DEPTH COMPARE FUNCTION
	sendRS(s, D3DRS_ZFUNC, Get_Depth_Compare() + 1);

	// DEPTH MASK
	sendRS(s, D3DRS_ZWRITEENABLE, Get_Depth_Mask());

	// CULLMODE
	sendRS(s, D3DRS_CULLMODE, Get_Cull_Mode() ? s.sg.polygonCullMode : D3DCULL_NONE);

	// NPATCHES (the GeneralsX Support_NPatches() guard, ZW/shader.cpp:1033-1051)
	if ((diff & MASK_NPATCHENABLE)
		&& s.sg.npatches) {
		float level = 1.0f;
		if (Get_NPatch_Enable()) level = float(npatchesLevel);
		uint32_t bitsOfLevel;
		std::memcpy(&bitsOfLevel, &level, sizeof(bitsOfLevel));	// *((DWORD*)&level)
		sendRS(s, D3DRS_PATCHSEGMENTS, bitsOfLevel);
	}

	// Enable/disable alpha test
	sendRS(s, D3DRS_ALPHATESTENABLE, Get_Alpha_Test());
}

} // namespace gxl2m
