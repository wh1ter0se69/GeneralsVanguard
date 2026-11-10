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

/***********************************************************************************************
 ***              C O N F I D E N T I A L  ---  W E S T W O O D  S T U D I O S               ***
 ***********************************************************************************************
 *                                                                                             *
 *                 Project Name : WW3D                                                         *
 *                                                                                             *
 *                     $Archive:: ww3d2/texturefilter.cpp												$*
 *                                                                                             *
 *                  $Org Author:: Kenny Mitchell                                              $*
 *                                                                                             *
 *                       Author : Kenny Mitchell                                               *
 *                                                                                             *
 *                     $Modtime:: 08/05/02 1:27p                                              $*
 *                                                                                             *
 *                    $Revision:: 1                                                          $*
 *                                                                                             *
 * 08/05/02 KM Texture filter class abstraction																			*
 *---------------------------------------------------------------------------------------------*
 * Functions:                                                                                  *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#include "texturefilter.h"
#include "dx8wrapper.h"

const char* const TextureFilterClass::TextureFilterModeString[TEXTURE_FILTER_COUNT] = {
	"None",
	"Point",
	"Bilinear",
	"Trilinear",
	"Anisotropic"
};

TextureFilterClass::TextureFilterMode TextureFilterClass::getTextureFilterMode(const char* str) {
	for (int i = 0; i < TextureFilterClass::TEXTURE_FILTER_COUNT; ++i) {
		if (stricmp(str, TextureFilterClass::TextureFilterModeString[i]) == 0) {
			return (TextureFilterClass::TextureFilterMode)i;
		}
	}

	return TextureFilterClass::TEXTURE_FILTER_NONE;
}

// GeneralsX: THE DEFAULT IS WHAT PLAYERS ACTUALLY SAW: ANISOTROPIC 16X.
//
// Every shipped dxvk.conf forced d3d9.samplerAnisotropy = 16, which DXVK applies to every mipped
// sampler above POINT whatever the game asked for (d3d9_device.cpp, BindSampler). So Bilinear,
// Trilinear and Anisotropic 2x-8x all drew as 16x, and that force was the only anisotropy the
// terrain ever got - its shaders set LINEAR themselves (W3DShaderManager.cpp TerrainShader2Stage::set).
// W3DDisplay::init now hands DXVK the chosen level instead, so every choice draws as named - which
// would have blurred the far ground for everyone on the default, because "Bilinear" was only sharp
// through that force.
//
// So only a pick the launcher wrote is honoured - it writes GXTextureFilterPicked beside every one - and
// any other file draws as the 16x it always drew as. The game's own Options screen has no filter
// control and saves back whatever the file held, Bilinear when it held nothing (OptionsMenu.cpp,
// "texture filter mode"), and the iOS seed says Anisotropic with no level (ios/config/Options.ini),
// which the getter reads as 2x. Picks made in the 0.9.52-0.9.56 launchers carry no marker either: they
// too draw as the 16x they drew as before, and the launcher shows the default until picked again -
// which only changes the picture for Point and None, the two filters the old force left alone.
void TextureFilterClass::Resolve_Launch_Choice(bool picked_in_launcher, TextureFilterMode& mode, AnisotropicFilterMode& level)
{
	if (!picked_in_launcher)
	{
		mode = TEXTURE_FILTER_ANISOTROPIC;
		level = TEXTURE_FILTER_ANISOTROPIC_16X;
	}
}

unsigned _MinTextureFilters[MAX_TEXTURE_STAGES][TextureFilterClass::FILTER_TYPE_COUNT];
unsigned _MagTextureFilters[MAX_TEXTURE_STAGES][TextureFilterClass::FILTER_TYPE_COUNT];
unsigned _MipMapFilters[MAX_TEXTURE_STAGES][TextureFilterClass::FILTER_TYPE_COUNT];

/*************************************************************************
**                             TextureFilterClass
*************************************************************************/
TextureFilterClass::TextureFilterClass(MipCountType mip_level_count)
:	TextureMinFilter(FILTER_TYPE_DEFAULT),
	TextureMagFilter(FILTER_TYPE_DEFAULT),
	UAddressMode(TEXTURE_ADDRESS_REPEAT),
	VAddressMode(TEXTURE_ADDRESS_REPEAT)
{
	if (mip_level_count!=MIP_LEVELS_1)
	{
		MipMapFilter=FILTER_TYPE_DEFAULT;
	}
	else
	{
		MipMapFilter=FILTER_TYPE_NONE;
	}
}

//**********************************************************************************************
//! Apply filters (legacy)
/*!
*/
void TextureFilterClass::Apply(unsigned int stage)
{
	DX8Wrapper::Set_DX8_Texture_Stage_State(stage,D3DTSS_MINFILTER,_MinTextureFilters[stage][TextureMinFilter]);
	DX8Wrapper::Set_DX8_Texture_Stage_State(stage,D3DTSS_MAGFILTER,_MagTextureFilters[stage][TextureMagFilter]);
	DX8Wrapper::Set_DX8_Texture_Stage_State(stage,D3DTSS_MIPFILTER,_MipMapFilters[stage][MipMapFilter]);

	switch (Get_U_Addr_Mode())
	{
	case TEXTURE_ADDRESS_REPEAT:
		DX8Wrapper::Set_DX8_Texture_Stage_State(stage, D3DTSS_ADDRESSU, D3DTADDRESS_WRAP);
		break;

	case TEXTURE_ADDRESS_CLAMP:
		DX8Wrapper::Set_DX8_Texture_Stage_State(stage, D3DTSS_ADDRESSU, D3DTADDRESS_CLAMP);
		break;
	}

	switch (Get_V_Addr_Mode())
	{
	case TEXTURE_ADDRESS_REPEAT:
		DX8Wrapper::Set_DX8_Texture_Stage_State(stage, D3DTSS_ADDRESSV, D3DTADDRESS_WRAP);
		break;

	case TEXTURE_ADDRESS_CLAMP:
		DX8Wrapper::Set_DX8_Texture_Stage_State(stage, D3DTSS_ADDRESSV, D3DTADDRESS_CLAMP);
		break;
	}
}

//**********************************************************************************************
//! Init filters (legacy)
/*!
*/
void TextureFilterClass::_Init_Filters(TextureFilterMode texture_filter, AnisotropicFilterMode anisotropy_level)
{
	const D3DCAPS8& dx8caps=DX8Wrapper::Get_Current_Caps()->Get_DX8_Caps();

	// TheSuperHackers @info Init zero stage filter defaults, point filtering is the lowest type for non mip filtering
	_MinTextureFilters[0][FILTER_TYPE_NONE]=D3DTEXF_POINT;
	_MagTextureFilters[0][FILTER_TYPE_NONE]=D3DTEXF_POINT;
	_MipMapFilters[0][FILTER_TYPE_NONE]=D3DTEXF_NONE;

	// Bilinear
	_MinTextureFilters[0][FILTER_TYPE_FAST]=D3DTEXF_LINEAR;
	_MagTextureFilters[0][FILTER_TYPE_FAST]=D3DTEXF_LINEAR;
	_MipMapFilters[0][FILTER_TYPE_FAST]=D3DTEXF_POINT;

	// Anisotropic - MipMap interlayer filtering only goes up to linear
	_MinTextureFilters[0][FILTER_TYPE_BEST]=D3DTEXF_ANISOTROPIC;
	_MagTextureFilters[0][FILTER_TYPE_BEST]=D3DTEXF_ANISOTROPIC;
	_MipMapFilters[0][FILTER_TYPE_BEST]=D3DTEXF_LINEAR;

	// TheSuperHackers @feature Mauller 08/03/2026 Add full support for all texture filtering modes;
	// None, Point, Bilinear, Trilinear, Anisotropic.
	BOOL FilterSupported = false;
	switch (texture_filter) {

	default:
		// TheSuperHackers @info if we have an invalid filter_type, set the filtering to none
		DEBUG_CRASH(("Invalid filter type passed into TextureFilterClass::_Init_Filters()"));
		FALLTHROUGH;

	case TEXTURE_FILTER_NONE:

		_MinTextureFilters[0][FILTER_TYPE_FAST]=D3DTEXF_POINT;
		_MagTextureFilters[0][FILTER_TYPE_FAST]=D3DTEXF_POINT;
		_MipMapFilters[0][FILTER_TYPE_FAST]=D3DTEXF_NONE;

		_MinTextureFilters[0][FILTER_TYPE_BEST]=D3DTEXF_POINT;
		_MagTextureFilters[0][FILTER_TYPE_BEST]=D3DTEXF_POINT;
		_MipMapFilters[0][FILTER_TYPE_BEST]=D3DTEXF_NONE;
		break;

	case TEXTURE_FILTER_POINT:

		_MinTextureFilters[0][FILTER_TYPE_FAST]=D3DTEXF_POINT;
		_MagTextureFilters[0][FILTER_TYPE_FAST]=D3DTEXF_POINT;
		_MipMapFilters[0][FILTER_TYPE_FAST]=D3DTEXF_POINT;

		_MinTextureFilters[0][FILTER_TYPE_BEST]=D3DTEXF_POINT;
		_MagTextureFilters[0][FILTER_TYPE_BEST]=D3DTEXF_POINT;
		_MipMapFilters[0][FILTER_TYPE_BEST]=D3DTEXF_POINT;
		break;

	case TEXTURE_FILTER_BILINEAR:

		FilterSupported = (dx8caps.TextureFilterCaps & D3DPTFILTERCAPS_MINFLINEAR) &&
			(dx8caps.TextureFilterCaps & D3DPTFILTERCAPS_MAGFLINEAR);

		if (FilterSupported) {
			_MinTextureFilters[0][FILTER_TYPE_BEST]=D3DTEXF_LINEAR;
			_MagTextureFilters[0][FILTER_TYPE_BEST]=D3DTEXF_LINEAR;
		}
		else {
			_MinTextureFilters[0][FILTER_TYPE_BEST]=D3DTEXF_POINT;
			_MagTextureFilters[0][FILTER_TYPE_BEST]=D3DTEXF_POINT;
		}

		_MipMapFilters[0][FILTER_TYPE_BEST]=D3DTEXF_POINT;
		break;

	case TEXTURE_FILTER_TRILINEAR:

		FilterSupported = (dx8caps.TextureFilterCaps & D3DPTFILTERCAPS_MINFLINEAR) &&
			(dx8caps.TextureFilterCaps & D3DPTFILTERCAPS_MAGFLINEAR);

		if (FilterSupported) {
			_MinTextureFilters[0][FILTER_TYPE_BEST]=D3DTEXF_LINEAR;
			_MagTextureFilters[0][FILTER_TYPE_BEST]=D3DTEXF_LINEAR;
		}
		else {
			_MinTextureFilters[0][FILTER_TYPE_BEST]=D3DTEXF_POINT;
			_MagTextureFilters[0][FILTER_TYPE_BEST]=D3DTEXF_POINT;
		}

		if (dx8caps.TextureFilterCaps & D3DPTFILTERCAPS_MIPFLINEAR) {
			_MipMapFilters[0][FILTER_TYPE_BEST]=D3DTEXF_LINEAR;
		}
		else {
			// TheSuperHackers @info if only linear mipmap filtering is unsupported,
			// Trilinear filtering becomes Bilinear filtering by default
			_MipMapFilters[0][FILTER_TYPE_BEST]=D3DTEXF_POINT;
		}
		break;

	case TEXTURE_FILTER_ANISOTROPIC:

		FilterSupported = (dx8caps.TextureFilterCaps & D3DPTFILTERCAPS_MAGFANISOTROPIC) &&
			(dx8caps.TextureFilterCaps & D3DPTFILTERCAPS_MINFANISOTROPIC);

		if (FilterSupported) {
			_MinTextureFilters[0][FILTER_TYPE_BEST]=D3DTEXF_ANISOTROPIC;
			_MagTextureFilters[0][FILTER_TYPE_BEST]=D3DTEXF_ANISOTROPIC;

			// Set the Anisotropic filtering level for all stages
			_Set_Max_Anisotropy(anisotropy_level);
		}
		else {
			_MinTextureFilters[0][FILTER_TYPE_BEST]=D3DTEXF_POINT;
			_MagTextureFilters[0][FILTER_TYPE_BEST]=D3DTEXF_POINT;
		}

		if (dx8caps.TextureFilterCaps & D3DPTFILTERCAPS_MIPFLINEAR) {
			_MipMapFilters[0][FILTER_TYPE_BEST]=D3DTEXF_LINEAR;
		}
		else {
			_MipMapFilters[0][FILTER_TYPE_BEST]=D3DTEXF_POINT;
		}
		break;

	}


	// For stages above zero, set best filter to the same as the stage zero
	int i=1;
	for (;i<MAX_TEXTURE_STAGES;++i) {
		_MinTextureFilters[i][FILTER_TYPE_NONE]=_MinTextureFilters[0][FILTER_TYPE_NONE];
		_MagTextureFilters[i][FILTER_TYPE_NONE]=_MagTextureFilters[0][FILTER_TYPE_NONE];
		_MipMapFilters[i][FILTER_TYPE_NONE]=_MipMapFilters[0][FILTER_TYPE_NONE];

		_MinTextureFilters[i][FILTER_TYPE_FAST]=_MinTextureFilters[0][FILTER_TYPE_FAST];
		_MagTextureFilters[i][FILTER_TYPE_FAST]=_MagTextureFilters[0][FILTER_TYPE_FAST];
		_MipMapFilters[i][FILTER_TYPE_FAST]=_MipMapFilters[0][FILTER_TYPE_FAST];

		// When Anisotropic filtering is used, all stages above zero use trilinear filtering
		if (_MagTextureFilters[0][FILTER_TYPE_BEST]==D3DTEXF_ANISOTROPIC) {
			_MagTextureFilters[i][FILTER_TYPE_BEST]=D3DTEXF_LINEAR;
		}
		else {
			_MagTextureFilters[i][FILTER_TYPE_BEST]=_MagTextureFilters[0][FILTER_TYPE_BEST];
		}

		if (_MinTextureFilters[0][FILTER_TYPE_BEST]==D3DTEXF_ANISOTROPIC) {
			_MinTextureFilters[i][FILTER_TYPE_BEST]=D3DTEXF_LINEAR;
		}
		else {
			_MinTextureFilters[i][FILTER_TYPE_BEST]=_MinTextureFilters[0][FILTER_TYPE_BEST];
		}
		_MipMapFilters[i][FILTER_TYPE_BEST]=_MipMapFilters[0][FILTER_TYPE_BEST];

	}

	// Set default to best. The level of best filter mode is controlled by the input parameter.
	for (i=0;i<MAX_TEXTURE_STAGES;++i) {
		_MinTextureFilters[i][FILTER_TYPE_DEFAULT]=_MinTextureFilters[i][FILTER_TYPE_BEST];
		_MagTextureFilters[i][FILTER_TYPE_DEFAULT]=_MagTextureFilters[i][FILTER_TYPE_BEST];
		_MipMapFilters[i][FILTER_TYPE_DEFAULT]=_MipMapFilters[i][FILTER_TYPE_BEST];
	}

}


//**********************************************************************************************
//! Set mip mapping filter (legacy)
/*!
*/
void TextureFilterClass::Set_Mip_Mapping(FilterType mipmap)
{
//	if (mipmap != FILTER_TYPE_NONE && Get_Mip_Level_Count() <= 1 && Is_Initialized())
//	{
//		WWASSERT_PRINT(0, "Trying to enable MipMapping on texture w/o Mip levels!");
//		return;
//	}
	MipMapFilter=mipmap;
}

//**********************************************************************************************
//! Set anisotropic filter level
/*!
*/
void TextureFilterClass::_Set_Max_Anisotropy(AnisotropicFilterMode mode)
{
	for (int stage = 0; stage < MAX_TEXTURE_STAGES; ++stage)
		DX8Wrapper::Set_DX8_Texture_Stage_State(stage, D3DTSS_MAXANISOTROPY, mode);
}

//**********************************************************************************************
//! Set default min filter (legacy)
/*!
*/
void TextureFilterClass::_Set_Default_Min_Filter(FilterType filter)
{
	for (int i=0;i<MAX_TEXTURE_STAGES;++i)
	{
		_MinTextureFilters[i][FILTER_TYPE_DEFAULT]=_MinTextureFilters[i][filter];
	}
}


//**********************************************************************************************
//! Set default mag filter (legacy)
/*!
*/
void TextureFilterClass::_Set_Default_Mag_Filter(FilterType filter)
{
	for (int i=0;i<MAX_TEXTURE_STAGES;++i)
	{
		_MagTextureFilters[i][FILTER_TYPE_DEFAULT]=_MagTextureFilters[i][filter];
	}
}

//**********************************************************************************************
//! Set default mip filter (legacy)
/*!
*/
void TextureFilterClass::_Set_Default_Mip_Filter(FilterType filter)
{
	for (int i=0;i<MAX_TEXTURE_STAGES;++i)
	{
		_MipMapFilters[i][FILTER_TYPE_DEFAULT]=_MipMapFilters[i][filter];
	}
}
