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

// RA3 WATER: engine glue for GameClient/GXWaterRA3.h. Render-only, below every GX-SIMID region.
// Tint pass: shore tiles redrawn with a tint LUT (blend DESTCOLOR/SRCALPHA, RGB only, no z write, no fog).
// Sheen passes (standing water, rivers) draw ADDED; device state is restored RAW, never through the wrapper caches.
// The 256x1 tint LUT and 128x128 surface texture are built on first use and refilled after a device reset.

namespace
{
	Bool s_gxRA3On = FALSE;							// the option, as read at the last map load (or set since)
	Bool s_gxRA3MapLoaded = FALSE;					// from WaterRenderObjClass::load to the map's teardown
	Bool s_gxRA3TintPass = FALSE;					// between gxWaterRA3TintBegin and gxWaterRA3TintEnd
	GxWaterRA3Tune s_gxRA3Tune = gxWaterRA3DefaultTune();
	const char *s_gxRA3Source = "default";
	TextureClass *s_gxRA3TintLUT = nullptr;			// 256 x 1: the sea floor's multiplier by depth
	TextureClass *s_gxRA3Surface = nullptr;			// 128 x 128 + mips: the smooth surface (flat)
	Bool s_gxRA3TintFilled = FALSE;
	Bool s_gxRA3SurfaceFilled = FALSE;
	Real s_gxRA3TintCap = -1.0f;					// the opacity cap the tint LUT was filled for
	Int s_gxRA3TintDraws = 0;						// tint passes drawn since the map loaded (the map-load line counts the last map's)
	Bool s_gxRA3SaidColour = FALSE;					// the once-per-map [GXWATER] ra3 colour / surface lines
	Bool s_gxRA3SaidSurface = FALSE;
	Int s_gxRA3SurfaceLevels = 0;					// mip levels the surface texture was filled with
	// phase 2, the motion
	TextureClass *s_gxRA3Normal = nullptr;			// 256 x 256 + mips: the ripples' normals
	Bool s_gxRA3NormalFilled = FALSE;
	Real s_gxRA3NormalBump = -1.0f;					// the bump it was filled with
	Int s_gxRA3NormalCells = 0;						// and the lattice
	Int s_gxRA3NormalLevels = 0;
	DWORD s_gxRA3SheenShader = 0;					// water:ra3 (kGxWaterRA3SheenShader), made at the first pass while on
	Bool s_gxRA3SheenFailed = FALSE;				// CreatePixelShader refused it: not asked again until the next map
	Int s_gxRA3SheenDraws = 0;						// sheen passes drawn since the map loaded
	Bool s_gxRA3SaidSheen = FALSE;					// the once-per-map [GXWATER] ra3 sheen line
	// phase 3, rivers
	TextureClass *s_gxRA3RiverSurface = nullptr;	// 128 x 128 + mips: the smooth surface at riverOpacity's alpha
	Bool s_gxRA3RiverSurfaceFilled = FALSE;
	Real s_gxRA3RiverOpacity = -1.0f;				// the alpha it was filled with
	DWORD s_gxRA3RiverShader = 0;					// water:ra3river (kGxWaterRA3RiverSheenShader), made at the first river pass while on
	Bool s_gxRA3RiverShaderFailed = FALSE;			// CreatePixelShader refused it: not asked again until the next map
	Int s_gxRA3RiverDraws = 0;						// river sheen passes drawn since the map loaded
	Bool s_gxRA3SaidRiver = FALSE;					// the once-per-map [GXWATER] ra3 river sheen line
	GxWaterRA3RiverVertex *s_gxRA3RiverMesh = nullptr;	// scratch for one river's mesh (6 rows - 2), grown as needed, freed at shutdown
	float (*s_gxRA3RiverBanks)[3] = nullptr;		// scratch: the rows' a points, then the b points
	Int s_gxRA3RiverCap = 0;						// rows the scratch holds
}

static void gxWaterRA3ReadSettings()
{
	OptionPreferences prefs;
	const OptionPreferences::const_iterator key = prefs.find("GXWaterRA3");	// absent = ON (GXWaterRA3.h)
	s_gxRA3On = gxWaterRA3OptionOn((key != prefs.end()) ? key->second.str() : nullptr);
	s_gxRA3Source = (key != prefs.end()) ? "Options.ini" : "default";
	const Int env = gxWaterRA3ParseBool(getenv("GX_WATER_RA3"));
	if (env >= 0)
	{
		s_gxRA3On = (env == 1);
		s_gxRA3Source = "GX_WATER_RA3";
	}
	s_gxRA3Tune = gxWaterRA3DefaultTune();
	gxWaterRA3ParseTune(getenv("GX_WATER_RA3_TUNE"), &s_gxRA3Tune);
}

bool gxWaterRA3Active()
{
	return s_gxRA3On && s_gxRA3MapLoaded && TheGlobalData != nullptr && !TheGlobalData->m_isWorldBuilder;
}

// ---------------------------------------------------------------------------------------------------------------------------
// Textures.

static void gxWaterRA3FillTint(Real cap)
{
	if (s_gxRA3TintLUT == nullptr)
		return;
	SurfaceClass *surf = s_gxRA3TintLUT->Get_Surface_Level();
	if (surf == nullptr)
		return;
	int pitch = 0;
	unsigned *bits = (unsigned *)surf->Lock(&pitch);
	if (bits != nullptr)
	{
		gxWaterRA3FillTintLUT(bits, cap, s_gxRA3Tune);
		surf->Unlock();
		s_gxRA3TintFilled = TRUE;
		s_gxRA3TintCap = cap;
	}
	REF_PTR_RELEASE(surf);
}

// Uploads `level` (size x size A8R8G8B8; overwritten) as level 0, each smaller level the 2 x 2 average of the one above, channel by
// channel (the tilings wrap, so does the average). Returns the levels written; *ok is FALSE if a level could not be locked.
static Int gxWaterRA3UploadMips(TextureClass *tex, unsigned *level, Int size, Bool *ok)
{
	// The D3D texture's own level count: Get_Mip_Level_Count() returns the constructor's request, and MIP_LEVELS_ALL is 0
	// (the loop uploaded nothing and the water sampled an uninitialised texture).
	const Int levels = (tex->Peek_D3D_Texture() != nullptr) ? (Int)tex->Peek_D3D_Texture()->GetLevelCount() : 0;
	*ok = levels >= 1 ? TRUE : FALSE;
	Int written = 0;
	for (Int l = 0; l < levels && size >= 1; ++l)
	{
		SurfaceClass *surf = tex->Get_Surface_Level(l);
		if (surf == nullptr)
		{
			*ok = FALSE;
			break;
		}
		int pitch = 0;
		UnsignedByte *bits = (UnsignedByte *)surf->Lock(&pitch);
		if (bits != nullptr)
		{
			for (Int y = 0; y < size; ++y)
				memcpy(bits + (size_t)y * (size_t)pitch, level + (size_t)y * (size_t)size, (size_t)size * sizeof(unsigned));
			surf->Unlock();
		}
		else
			*ok = FALSE;
		REF_PTR_RELEASE(surf);
		written = l + 1;
		if (size == 1)
			break;
		const Int half = size / 2;
		for (Int y = 0; y < half; ++y)
			for (Int x = 0; x < half; ++x)
			{
				const unsigned a = level[(2 * y) * size + 2 * x], b = level[(2 * y) * size + 2 * x + 1];
				const unsigned c = level[(2 * y + 1) * size + 2 * x], d = level[(2 * y + 1) * size + 2 * x + 1];
				unsigned out = 0;
				for (Int sh = 0; sh < 32; sh += 8)
					out |= ((((a >> sh) & 0xff) + ((b >> sh) & 0xff) + ((c >> sh) & 0xff) + ((d >> sh) & 0xff) + 2) / 4) << sh;
				level[y * half + x] = out;
			}
		size = half;
	}
	return written;
}

// Level 0 from GXWaterRA3.h, its mips the average (a grey stays the same grey: phase 1's bytes exactly).
static void gxWaterRA3FillSurfaceTexture()
{
	if (s_gxRA3Surface == nullptr)
		return;
	enum { N = 128 };
	unsigned *level = new unsigned[ N * N ];
	gxWaterRA3FillSurface(level, N);
	Bool ok = FALSE;
	s_gxRA3SurfaceLevels = gxWaterRA3UploadMips(s_gxRA3Surface, level, N, &ok);
	delete [] level;
	s_gxRA3SurfaceFilled = ok;
}

// The ripples' normal map (phase 2): GXWaterRA3.h's, at this map's bump; its mips average the normals, so the ripples flatten with
// distance instead of shimmering.
static void gxWaterRA3FillNormalTexture()
{
	if (s_gxRA3Normal == nullptr)
		return;
	enum { N = 256 };
	unsigned *level = new unsigned[ N * N ];
	const Int cells = (Int)(s_gxRA3Tune.cells + 0.5f);
	gxWaterRA3FillNormalMap(level, N, s_gxRA3Tune.bump, cells);
	Bool ok = FALSE;
	s_gxRA3NormalLevels = gxWaterRA3UploadMips(s_gxRA3Normal, level, N, &ok);
	delete [] level;
	s_gxRA3NormalFilled = ok;
	s_gxRA3NormalBump = s_gxRA3Tune.bump;
	s_gxRA3NormalCells = cells;
}

static void gxWaterRA3MakeTextures()
{
	if (s_gxRA3TintLUT == nullptr)
	{
		s_gxRA3TintLUT = MSGNEW("TextureClass") TextureClass(256, 1, WW3D_FORMAT_A8R8G8B8, MIP_LEVELS_1);
		s_gxRA3TintLUT->Get_Filter().Set_U_Addr_Mode(TextureFilterClass::TEXTURE_ADDRESS_CLAMP);
		s_gxRA3TintLUT->Get_Filter().Set_V_Addr_Mode(TextureFilterClass::TEXTURE_ADDRESS_CLAMP);
		s_gxRA3TintLUT->Get_Filter().Set_Mag_Filter(TextureFilterClass::FILTER_TYPE_DEFAULT);
		s_gxRA3TintLUT->Get_Filter().Set_Min_Filter(TextureFilterClass::FILTER_TYPE_DEFAULT);
		s_gxRA3TintLUT->Get_Filter().Set_Mip_Mapping(TextureFilterClass::FILTER_TYPE_NONE);
		s_gxRA3TintFilled = FALSE;
	}
	if (s_gxRA3Surface == nullptr && s_gxRA3Tune.flat >= 0.5f)
	{
		s_gxRA3Surface = MSGNEW("TextureClass") TextureClass(128, 128, WW3D_FORMAT_A8R8G8B8, MIP_LEVELS_ALL);
		s_gxRA3Surface->Get_Filter().Set_U_Addr_Mode(TextureFilterClass::TEXTURE_ADDRESS_REPEAT);
		s_gxRA3Surface->Get_Filter().Set_V_Addr_Mode(TextureFilterClass::TEXTURE_ADDRESS_REPEAT);
		s_gxRA3Surface->Get_Filter().Set_Mag_Filter(TextureFilterClass::FILTER_TYPE_BEST);
		s_gxRA3Surface->Get_Filter().Set_Min_Filter(TextureFilterClass::FILTER_TYPE_BEST);
		s_gxRA3Surface->Get_Filter().Set_Mip_Mapping(TextureFilterClass::FILTER_TYPE_BEST);
		s_gxRA3SurfaceFilled = FALSE;
	}
}

static void gxWaterRA3MakeNormalTexture()
{
	if (s_gxRA3Normal != nullptr)
		return;
	s_gxRA3Normal = MSGNEW("TextureClass") TextureClass(256, 256, WW3D_FORMAT_A8R8G8B8, MIP_LEVELS_ALL);
	s_gxRA3Normal->Get_Filter().Set_U_Addr_Mode(TextureFilterClass::TEXTURE_ADDRESS_REPEAT);
	s_gxRA3Normal->Get_Filter().Set_V_Addr_Mode(TextureFilterClass::TEXTURE_ADDRESS_REPEAT);
	s_gxRA3Normal->Get_Filter().Set_Mag_Filter(TextureFilterClass::FILTER_TYPE_BEST);
	s_gxRA3Normal->Get_Filter().Set_Min_Filter(TextureFilterClass::FILTER_TYPE_BEST);
	s_gxRA3Normal->Get_Filter().Set_Mip_Mapping(TextureFilterClass::FILTER_TYPE_BEST);
	s_gxRA3NormalFilled = FALSE;
}

static void gxWaterRA3ReleaseSheenShader()
{
	IDirect3DDevice8 *dev = DX8Wrapper::_Get_D3D_Device8();
	if (dev != nullptr && s_gxRA3SheenShader != 0)
		dev->DeletePixelShader(s_gxRA3SheenShader);
	s_gxRA3SheenShader = 0;
	if (dev != nullptr && s_gxRA3RiverShader != 0)
		dev->DeletePixelShader(s_gxRA3RiverShader);
	s_gxRA3RiverShader = 0;
}

// ReleaseResources (a device reset): the textures keep their objects but not their contents; the program is made again at the
// next pass, as live water's two are at ReAcquireResources.
static void gxWaterRA3Release()
{
	s_gxRA3TintFilled = FALSE;
	s_gxRA3SurfaceFilled = FALSE;
	s_gxRA3NormalFilled = FALSE;
	s_gxRA3RiverSurfaceFilled = FALSE;
	gxWaterRA3ReleaseSheenShader();
}

// The water object's destruction.
static void gxWaterRA3Shutdown()
{
	REF_PTR_RELEASE(s_gxRA3TintLUT);
	REF_PTR_RELEASE(s_gxRA3Surface);
	REF_PTR_RELEASE(s_gxRA3Normal);
	REF_PTR_RELEASE(s_gxRA3RiverSurface);
	gxWaterRA3ReleaseSheenShader();
	s_gxRA3TintFilled = FALSE;
	s_gxRA3SurfaceFilled = FALSE;
	s_gxRA3NormalFilled = FALSE;
	s_gxRA3RiverSurfaceFilled = FALSE;
	s_gxRA3MapLoaded = FALSE;
	delete [] s_gxRA3RiverMesh;
	delete [] s_gxRA3RiverBanks;
	s_gxRA3RiverMesh = nullptr;
	s_gxRA3RiverBanks = nullptr;
	s_gxRA3RiverCap = 0;
}

// ---------------------------------------------------------------------------------------------------------------------------
// Map lifetime.

void gxWaterRA3MapLoaded()
{
	if (TheGlobalData != nullptr && TheGlobalData->m_isWorldBuilder)
		return;
	const Int lastDraws = s_gxRA3TintDraws;
	const Int lastSheen = s_gxRA3SheenDraws;
	const Int lastRiver = s_gxRA3RiverDraws;
	gxWaterRA3ReadSettings();
	s_gxRA3MapLoaded = TRUE;
	s_gxRA3TintDraws = 0;
	s_gxRA3SheenDraws = 0;
	s_gxRA3RiverDraws = 0;
	s_gxRA3SheenFailed = FALSE;	// a new map asks for the program again
	s_gxRA3RiverShaderFailed = FALSE;
	s_gxRA3SaidSheen = FALSE;
	s_gxRA3SaidRiver = FALSE;
	s_gxRA3TintFilled = FALSE;	// the tune may have changed: refill at the first tint pass
	s_gxRA3SaidColour = FALSE;
	s_gxRA3SaidSurface = FALSE;
	const GxWaterRA3Tune &t = s_gxRA3Tune;
	fprintf(stderr, "[GXWATER] ra3 %s src=%s reach=%.2f opacity=%.2f shape=%.2f tint=%.2f/%.2f/%.2f tintShape=%.2f"
		" deep=%.2f/%.2f/%.2f flat=%d lastMapTintDraws=%d\n",
		s_gxRA3On ? "on" : "off", s_gxRA3Source, t.reach, t.opacity, t.shape, t.tintR, t.tintG, t.tintB, t.tintShape, t.deepR,
		t.deepG, t.deepB, t.flat >= 0.5f ? 1 : 0, lastDraws);
	fprintf(stderr, "[GXWATER] ra3 motion %s glint=%.2f tilt=%.0f-%.0f sky=%.2f skyColour=%.2f/%.2f/%.2f scale=%.0f speed=%.1f div=%.1f"
		" bump=%.2f cells=%.0f lastMapSheenDraws=%d\n", (s_gxRA3On && gxWaterRA3SheenOn(t)) ? "on" : "off", t.glint, t.tiltMin, t.tiltMax,
		t.sky, t.skyR, t.skyG, t.skyB, t.scale, t.speed, t.div, t.bump, t.cells, lastSheen);
	fprintf(stderr, "[GXWATER] ra3 rivers %s opacity=%.2f speed=%.1f shade=%.2f sky=%.2f lastMapRiverDraws=%d\n",
		(s_gxRA3On && gxWaterRA3RiverOn(t)) ? "on" : "off", t.riverOpacity, t.riverSpeed, t.riverShade, t.riverSky, lastRiver);
}

void gxWaterRA3MapReset()
{
	s_gxRA3MapLoaded = FALSE;
	s_gxRA3TintPass = FALSE;
}

void gxWaterRA3Set(bool on)
{
	s_gxRA3On = on ? TRUE : FALSE;
	if (!s_gxRA3MapLoaded)
		return;
	s_gxRA3TintFilled = FALSE;
	gxWaterClearApplyNow("ra3");	// the override values (GXWaterClearGlue.inl, earlier in this file)
	if (TheTerrainRenderObject != nullptr)
		TheTerrainRenderObject->setShoreLineDetail();	// the tile set and the LUT's shape, even when the cap did not move
}

bool gxWaterRA3OverrideValues(float depth, float minOpacity, float *outDepth, float *outMinOpacity)
{
	if (!gxWaterRA3Active() || outDepth == nullptr || outMinOpacity == nullptr)
		return false;
	const GxWaterRA3Values v = gxWaterRA3Apply(depth, minOpacity, s_gxRA3Tune);
	*outDepth = v.depth;
	*outMinOpacity = v.minOpacity;
	return true;
}

bool gxWaterRA3FillLUT(unsigned *texels256, float cap)
{
	if (!gxWaterRA3Active() || texels256 == nullptr)
		return false;
	gxWaterRA3FillOpacityLUT(texels256, cap, s_gxRA3Tune.shape);
	return true;
}

unsigned gxWaterRA3LutKey()
{
	if (!gxWaterRA3Active())
		return 0;
	return 0x80000000u | (unsigned)(gxWaterRA3Clamp(s_gxRA3Tune.shape, 0.0f, 8.0f) * 4096.0f);
}

// ---------------------------------------------------------------------------------------------------------------------------
// The tint pass.

bool gxWaterRA3TintPassOn()
{
	return s_gxRA3TintPass != FALSE;
}

bool gxWaterRA3TintBegin()
{
	if (!gxWaterRA3Active() || s_gxRA3TintPass)
		return false;
	// Where retail draws no soft shore (gxWaterRA3.h review focus 4), there is no depth to tint by.
	if (!TheGlobalData->m_showSoftWaterEdge || TheWaterTransparency == nullptr || TheWaterTransparency->m_transparentWaterDepth == 0
		|| DX8Wrapper::getBackBufferFormat() != WW3D_FORMAT_A8R8G8B8)
		return false;
	if (ShaderClass::Is_Backface_Culling_Inverted())
		return false;	// a mirror pass
	gxWaterRA3MakeTextures();
	if (s_gxRA3TintLUT == nullptr)
		return false;
	const Real cap = TheWaterTransparency->m_minWaterOpacity;
	if (!s_gxRA3TintFilled || s_gxRA3TintCap != cap)
		gxWaterRA3FillTint(cap);
	if (!s_gxRA3TintFilled)
		return false;

	ShaderClass shader = ShaderClass::_PresetOpaque2DShader;
	shader.Set_Depth_Compare(ShaderClass::PASS_LEQUAL);
	DX8Wrapper::Set_Shader(shader);
	VertexMaterialClass *vmat = VertexMaterialClass::Get_Preset(VertexMaterialClass::PRELIT_DIFFUSE);
	DX8Wrapper::Set_Material(vmat);
	REF_PTR_RELEASE(vmat);
	DX8Wrapper::Set_Texture(0, s_gxRA3TintLUT);
	DX8Wrapper::Set_Texture(1, nullptr);
	DX8Wrapper::Set_Transform(D3DTS_WORLD, Matrix3D(true));
	DX8Wrapper::Apply_Render_State_Changes();

	// Over the shader's own states (ShaderClass::Invalidate in gxWaterRA3TintEnd makes the next shader set them all again).
	DX8Wrapper::Set_DX8_Render_State(D3DRS_ALPHABLENDENABLE, TRUE);
	DX8Wrapper::Set_DX8_Render_State(D3DRS_SRCBLEND, D3DBLEND_DESTCOLOR);
	DX8Wrapper::Set_DX8_Render_State(D3DRS_DESTBLEND, D3DBLEND_SRCALPHA);
	DX8Wrapper::Set_DX8_Render_State(D3DRS_ALPHATESTENABLE, FALSE);
	DX8Wrapper::Set_DX8_Render_State(D3DRS_ZWRITEENABLE, FALSE);
	DX8Wrapper::Set_DX8_Render_State(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
	DX8Wrapper::Set_DX8_Render_State(D3DRS_FOGENABLE, FALSE);
	DX8Wrapper::Set_DX8_Render_State(D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_BLUE | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_RED);
	DX8Wrapper::Set_DX8_Texture_Stage_State(0, D3DTSS_TEXCOORDINDEX, 0);
	DX8Wrapper::Set_DX8_Texture_Stage_State(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
	DX8Wrapper::Set_DX8_Texture_Stage_State(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
	DX8Wrapper::Set_DX8_Texture_Stage_State(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
	DX8Wrapper::Set_DX8_Texture_Stage_State(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
	DX8Wrapper::Set_DX8_Texture_Stage_State(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
	DX8Wrapper::Set_DX8_Texture_Stage_State(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
	DX8Wrapper::Set_DX8_Texture_Stage_State(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
	s_gxRA3TintPass = TRUE;
	++s_gxRA3TintDraws;
	return true;
}

void gxWaterRA3TintEnd()
{
	if (!s_gxRA3TintPass)
		return;
	s_gxRA3TintPass = FALSE;
	DX8Wrapper::Set_DX8_Render_State(D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_BLUE | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_RED);
	DX8Wrapper::Set_DX8_Render_State(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
	DX8Wrapper::Set_DX8_Render_State(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
	DX8Wrapper::Set_DX8_Render_State(D3DRS_ZWRITEENABLE, TRUE);
	DX8Wrapper::Set_DX8_Texture_Stage_State(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
	DX8Wrapper::Set_DX8_Texture_Stage_State(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
	DX8Wrapper::Set_Texture(0, nullptr);
	ShaderClass::Invalidate();
}

// ---------------------------------------------------------------------------------------------------------------------------
// The water surface (drawTrapezoidWater, setupFlatWaterShader).

// The water pass's vertex colour: the deep colour lit by the map, the retail alpha byte kept. Off: retail's own.
static unsigned gxWaterRA3WaterDiffuse(unsigned retail)
{
	if (!gxWaterRA3Active())
		return retail;
	float amb[3], dif[3];
	gxWaterLiveLights(amb, dif);	// GXWaterLiveGlue.inl, earlier in this file: the terrain's first light
	const unsigned ra3 = gxWaterRA3DeepDiffuse(amb, dif, s_gxRA3Tune, retail);
	if (!s_gxRA3SaidColour)
	{
		s_gxRA3SaidColour = TRUE;
		fprintf(stderr, "[GXWATER] ra3 colour ambient=%.2f/%.2f/%.2f diffuse=%.2f/%.2f/%.2f retail=0x%08x ra3=0x%08x\n", amb[0], amb[1], amb[2],
			dif[0], dif[1], dif[2], retail, ra3);
	}
	return ra3;
}

// Stage 0's texture: our smooth surface while the option is on with flat set; otherwise (or if it could not be made) retail's.
static TextureClass *gxWaterRA3SurfaceTexture(TextureClass *retail)
{
	if (!gxWaterRA3Active() || s_gxRA3Tune.flat < 0.5f)
		return retail;
	gxWaterRA3MakeTextures();
	if (s_gxRA3Surface != nullptr && !s_gxRA3SurfaceFilled)
		gxWaterRA3FillSurfaceTexture();
	TextureClass *bound = (s_gxRA3Surface != nullptr && s_gxRA3SurfaceFilled) ? s_gxRA3Surface : retail;
	if (!s_gxRA3SaidSurface)
	{
		s_gxRA3SaidSurface = TRUE;
		fprintf(stderr, "[GXWATER] ra3 surface %s (made=%d filled=%d levels=%d of %d)\n", bound == retail ? "RETAIL" : "ours",
			s_gxRA3Surface != nullptr ? 1 : 0, s_gxRA3SurfaceFilled ? 1 : 0, (int)s_gxRA3SurfaceLevels,
			(s_gxRA3Surface != nullptr && s_gxRA3Surface->Peek_D3D_Texture() != nullptr) ? (int)s_gxRA3Surface->Peek_D3D_Texture()->GetLevelCount() : 0);
	}
	return bound;
}

// ---------------------------------------------------------------------------------------------------------------------------
// Phase 2, the motion: the sheen pass (renderWater, right after drawTrapezoidWater drew the patch).

// One texture-stage state this pass changes: read from the device before, put back raw after.
struct GxRA3SavedTSS
{
	DWORD stage;
	D3DTEXTURESTAGESTATETYPE type;
	DWORD value;
};

// The standing-water patch drawTrapezoidWater just drew (`points`, the same four corners), drawn once more from vertex and index
// buffers of its own - the same grid and positions (drawTrapezoidWater's own arithmetic), the tilings' coordinates, the sky x
// Fresnel. Called from renderWater AFTER drawTrapezoidWater returns: its dynamic buffers are released by then (the shared dynamic
// buffer moves its offset only when an access ends, so an allocation while drawTrapezoidWater's was alive overlapped the range the
// water had just drawn from, locked NOOVERWRITE). So this pass sets up its own shroud on stage 3 as live
// water's foam does. `stage0` / `stage1` are the textures setupFlatWaterShader bound there (the surface, the sparkles) and `white`
// the stage-3 stand-in without a shroud.
static void gxWaterRA3DrawSheen(const Vector3 points[4], TextureClass *stage0, TextureClass *stage1, TextureClass *white)
{
	if (!gxWaterRA3Active() || !gxWaterRA3SheenOn(s_gxRA3Tune) || stage0 == nullptr || stage1 == nullptr || white == nullptr)
		return;
	if (W3DShaderManager::getChipset() < DC_GENERIC_PIXEL_SHADER_1_1 || ShaderClass::Is_Backface_Culling_Inverted()
		|| TheTerrainRenderObject == nullptr)
		return;	// no ps.1.1 (no platform we ship), or a mirror pass
	IDirect3DDevice8 *dev = DX8Wrapper::_Get_D3D_Device8();
	if (dev == nullptr)
		return;
	if (s_gxRA3SheenShader == 0)
	{
		if (s_gxRA3SheenFailed)
			return;
		if (FAILED(dev->CreatePixelShader((const DWORD *)kGxWaterRA3SheenShader, &s_gxRA3SheenShader)))
		{
			s_gxRA3SheenShader = 0;
			s_gxRA3SheenFailed = TRUE;
			fprintf(stderr, "[GXWATER] ra3 sheen: CreatePixelShader FAILED - no motion on this map\n");
			return;
		}
	}
	gxWaterRA3MakeNormalTexture();
	if (s_gxRA3Normal == nullptr)
		return;
	if (!s_gxRA3NormalFilled || s_gxRA3NormalBump != s_gxRA3Tune.bump || s_gxRA3NormalCells != (Int)(s_gxRA3Tune.cells + 0.5f))
		gxWaterRA3FillNormalTexture();
	if (!white->Is_Initialized())
		return;	// setupFlatWaterShader made and filled it white for this patch already; never a blank one here
	if (!s_gxRA3NormalFilled || s_gxRA3Normal->Peek_D3D_Texture() == nullptr || stage0->Peek_D3D_Texture() == nullptr
		|| stage1->Peek_D3D_Texture() == nullptr || white->Peek_D3D_Texture() == nullptr)
		return;

	// drawTrapezoidWater's grid, its arithmetic exactly: the corner vectors, the counts (80-unit cells, at most 50 x 50).
	Vector3 origin(points[0]);
	Vector3 uVec1(points[1]);
	Vector3 vVec1(points[3]);
	Vector3 uVec2(points[2]);
	Vector3 vVec2(points[2]);
	uVec2 -= vVec1;
	vVec2 -= uVec1;
	uVec1 -= origin;
	vVec1 -= origin;
	Int uCount = (uVec1.Length() + uVec2.Length()) / (8 * MAP_XY_FACTOR);
	if (uCount < 1) uCount = 1;
	Int vCount = (vVec1.Length() + vVec2.Length()) / (8 * MAP_XY_FACTOR);
	if (vCount < 1) vCount = 1;
	if (uCount > 50) uCount = 50;
	if (vCount > 50) vCount = 50;
	const Int rectangleCount = uCount * vCount;
	uCount++;
	vCount++;

	// The camera, from the view transform: its position (the inverse's translation) and the way it looks (a WW3D camera looks down
	// its own -z; the sign is taken so the look points down at the water whatever the convention).
	Matrix4x4 curView;
	DX8Wrapper::_Get_DX8_Transform(D3DTS_VIEW, curView);
	D3DXMATRIX inv;
	float det;
	D3DXMatrixInverse(&inv, &det, (D3DXMATRIX *)&curView);
	const GxWaterRA3Vec eye = { inv._41, inv._42, inv._43 };
	GxWaterRA3Vec look = { inv._31, inv._32, inv._33 };
	if (look.z > 0.0f)
		look = GxWaterRA3Vec{ -look.x, -look.y, -look.z };
	// The sun: the terrain's first light travels along m_terrainLightPos (retail lights the water with -z of it).
	const Coord3D &lp = TheGlobalData->m_terrainLightPos[0];
	const GxWaterRA3Vec h = gxWaterRA3HalfVector(GxWaterRA3Vec{ -lp.x, -lp.y, -lp.z }, GxWaterRA3Vec{ -look.x, -look.y, -look.z },
		s_gxRA3Tune.tiltMin, s_gxRA3Tune.tiltMax);
	float amb[3], dif[3];
	gxWaterLiveLights(amb, dif);	// GXWaterLiveGlue.inl, earlier in this file: the terrain's first light
	float sun[4];
	gxWaterRA3SunConst(dif, s_gxRA3Tune, sun);
	const GxWaterLiveFlow flow = gxWaterLiveFlowDir(s_gxLiveTune.dir);	// live water's one flow (its default when live is off)
	GxWaterRA3Tiling tiling[2];
	gxWaterRA3Tilings(s_gxLiveClock, flow.x, flow.y, s_gxRA3Tune, tiling);	// live water's clock: game seconds

	// The grid: drawTrapezoidWater's own index order and positions (the same bilinear patch, the same expression), the two tilings,
	// the sky.
	DynamicIBAccessClass ib_access(BUFFER_TYPE_DYNAMIC_DX8, (rectangleCount + 1) * 2 * 3);
	{
		DynamicIBAccessClass::WriteLockClass lockib(&ib_access);
		UnsignedShort *curIb = lockib.Get_Index_Array();
		if (curIb == nullptr)
			return;
		for (Int j = 0; j < vCount - 1; ++j)
			for (Int i = 0; i < uCount - 1; ++i)
			{
				curIb[0] = (UnsignedShort)(j * uCount + i);
				curIb[1] = (UnsignedShort)((j + 1) * uCount + i + 1);
				curIb[2] = (UnsignedShort)((j + 1) * uCount + i);
				curIb[3] = (UnsignedShort)(j * uCount + i);
				curIb[4] = (UnsignedShort)(j * uCount + i + 1);
				curIb[5] = (UnsignedShort)((j + 1) * uCount + i + 1);
				curIb += 6;
			}
	}
	DynamicVBAccessClass vb_access(BUFFER_TYPE_DYNAMIC_DX8, dynamic_fvf_type, (rectangleCount + 1) * 2);
	{
		DynamicVBAccessClass::WriteLockClass lock(&vb_access);
		VertexFormatXYZNDUV2 *vb = lock.Get_Formatted_Vertex_Array();
		if (vb == nullptr)
			return;
		const Real constE = 1.0f / (Real)(vCount - 1);
		const Real constF = 1.0f / (Real)(uCount - 1);
		for (Int j = 0; j < vCount; ++j)
		{
			const Real dv = (Real)j * constE;
			for (Int i = 0; i < uCount; ++i)
			{
				const Real du = (Real)i * constF;
				Vector3 vertex = origin;
				vertex += uVec1 * du;
				vertex += vVec1 * dv;
				vertex += (dv) * (du) * (vVec2 - vVec1);
				vb->x = vertex.X;
				vb->y = vertex.Y;
				vb->z = vertex.Z;
				vb->nx = 0.0f;
				vb->ny = 0.0f;
				vb->nz = 1.0f;
				vb->diffuse = gxWaterRA3SkyDiffuse(amb, dif, s_gxRA3Tune, gxWaterRA3Fresnel(eye, vertex.X, vertex.Y, vertex.Z));
				gxWaterRA3TilingUV(tiling[0], vertex.X, vertex.Y, &vb->u1, &vb->v1);
				gxWaterRA3TilingUV(tiling[1], vertex.X, vertex.Y, &vb->u2, &vb->v2);
				++vb;
			}
		}
	}

	if (!s_gxRA3SaidSheen)
	{
		s_gxRA3SaidSheen = TRUE;
		const float tilt = acosf(h.z > 1.0f ? 1.0f : h.z) * 57.2957795f;
		const unsigned skyMid = gxWaterRA3SkyDiffuse(amb, dif, s_gxRA3Tune, gxWaterRA3Fresnel(eye, look.x * 1000.0f + eye.x,
			look.y * 1000.0f + eye.y, eye.z + look.z * 1000.0f));
		fprintf(stderr, "[GXWATER] ra3 sheen shader=0x%x normal=%d/%d levels eye=%.0f/%.0f/%.0f look=%.2f/%.2f/%.2f toSun=%.2f/%.2f/%.2f"
			" half=%.2f/%.2f/%.2f tilt=%.1f sun=%.2f/%.2f/%.2f skyAhead=0x%08x flow=%.2f/%.2f\n", (unsigned)s_gxRA3SheenShader,
			(int)s_gxRA3NormalLevels, (int)s_gxRA3Normal->Peek_D3D_Texture()->GetLevelCount(), eye.x, eye.y, eye.z, look.x, look.y,
			look.z, -lp.x, -lp.y, -lp.z, h.x, h.y, h.z, tilt, sun[0], sun[1], sun[2], skyMid, flow.x, flow.y);
	}

	// The shroud on stage 3, exactly as live water's foam pass sets it after the water (ShroudTextureShader::set applies the wrapper's
	// pending state itself, so the draw below applies only this pass's buffers).
	DX8Wrapper::Set_Transform(D3DTS_WORLD, Matrix3D(true));
	W3DShroud *shroud = TheTerrainRenderObject->getShroud();
	if (shroud)
	{
		W3DShaderManager::setTexture(0, shroud->getShroudTexture());
		W3DShaderManager::setShader(W3DShaderManager::ST_SHROUD_TEXTURE, 3);
		GxRawRS(dev, D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
	}
	else
		dev->SetTexture(3, white->Peek_D3D_Texture());

	// Save what else this pass changes, from the DEVICE (the wrapper's caches can be stale here, and are left as they are).
	GxRA3SavedTSS saved[] = {
		{ 0, D3DTSS_TEXCOORDINDEX, 0 }, { 0, D3DTSS_TEXTURETRANSFORMFLAGS, 0 }, { 0, D3DTSS_ADDRESSU, 0 }, { 0, D3DTSS_ADDRESSV, 0 },
		{ 0, D3DTSS_MINFILTER, 0 }, { 0, D3DTSS_MAGFILTER, 0 }, { 0, D3DTSS_MIPFILTER, 0 },
		{ 1, D3DTSS_TEXCOORDINDEX, 0 }, { 1, D3DTSS_TEXTURETRANSFORMFLAGS, 0 }, { 1, D3DTSS_ADDRESSU, 0 }, { 1, D3DTSS_ADDRESSV, 0 },
		{ 1, D3DTSS_MINFILTER, 0 }, { 1, D3DTSS_MAGFILTER, 0 }, { 1, D3DTSS_MIPFILTER, 0 },
		{ 0, D3DTSS_MAXANISOTROPY, 0 }, { 1, D3DTSS_MAXANISOTROPY, 0 } };
	for (GxRA3SavedTSS &v : saved)
		dev->GetTextureStageState(v.stage, v.type, &v.value);
	DWORD srcBlend = D3DBLEND_SRCALPHA, destBlend = D3DBLEND_INVSRCALPHA, fogColour = 0, blendOn = TRUE, cull = D3DCULL_NONE;
	dev->GetRenderState(D3DRS_SRCBLEND, &srcBlend);
	dev->GetRenderState(D3DRS_DESTBLEND, &destBlend);
	dev->GetRenderState(D3DRS_FOGCOLOR, &fogColour);
	dev->GetRenderState(D3DRS_ALPHABLENDENABLE, &blendOn);
	dev->GetRenderState(D3DRS_CULLMODE, &cull);

	// Stages 0 and 1: the normal map by tiling A (u1/v1) and B (u2/v2), wrapped, filtered with its mips, anisotropic: the water is
	// seen at a grazing angle, and plain trilinear blurred the ripples' highlights into soft blotches there.
	// Stage 3 keeps the shroud.
	for (DWORD st = 0; st < 2; ++st)
	{
		dev->SetTexture(st, s_gxRA3Normal->Peek_D3D_Texture());
		GxRawTSS(dev, st, D3DTSS_TEXCOORDINDEX, st);
		GxRawTSS(dev, st, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
		GxRawTSS(dev, st, D3DTSS_ADDRESSU, D3DTADDRESS_WRAP);
		GxRawTSS(dev, st, D3DTSS_ADDRESSV, D3DTADDRESS_WRAP);
		GxRawTSS(dev, st, D3DTSS_MINFILTER, D3DTEXF_ANISOTROPIC);
		GxRawTSS(dev, st, D3DTSS_MAGFILTER, D3DTEXF_LINEAR);
		GxRawTSS(dev, st, D3DTSS_MIPFILTER, D3DTEXF_LINEAR);
		GxRawTSS(dev, st, D3DTSS_MAXANISOTROPY, 8);
	}
	dev->SetPixelShaderConstant(0, D3DXVECTOR4(h.x, h.y, h.z, 0.0f), 1);
	dev->SetPixelShaderConstant(1, D3DXVECTOR4(sun[0], sun[1], sun[2], sun[3]), 1);
	GxRawRS(dev, D3DRS_ALPHABLENDENABLE, TRUE);
	GxRawRS(dev, D3DRS_SRCBLEND, D3DBLEND_ONE);
	GxRawRS(dev, D3DRS_DESTBLEND, D3DBLEND_ONE);
	GxRawRS(dev, D3DRS_FOGCOLOR, 0);	// an added term fogs toward black, never toward the fog's colour on top of the water's own
	GxRawRS(dev, D3DRS_CULLMODE, D3DCULL_NONE);	// as drawTrapezoidWater draws the water
	dev->SetPixelShader(s_gxRA3SheenShader);
	DX8Wrapper::Set_Index_Buffer(ib_access, 0);
	DX8Wrapper::Set_Vertex_Buffer(vb_access);
	DX8Wrapper::Draw_Triangles(0, (unsigned short)(rectangleCount * 2), 0, (unsigned short)((rectangleCount + 1) * 2));
	++s_gxRA3SheenDraws;

	// Back as it was: drawTrapezoidWater's own end state.
	dev->SetPixelShader(0);
	dev->SetTexture(0, stage0->Peek_D3D_Texture());
	dev->SetTexture(1, stage1->Peek_D3D_Texture());
	for (const GxRA3SavedTSS &v : saved)
		GxRawTSS(dev, v.stage, v.type, v.value);
	GxRawRS(dev, D3DRS_SRCBLEND, srcBlend);
	GxRawRS(dev, D3DRS_DESTBLEND, destBlend);
	GxRawRS(dev, D3DRS_FOGCOLOR, fogColour);
	GxRawRS(dev, D3DRS_ALPHABLENDENABLE, blendOn);
	GxRawRS(dev, D3DRS_CULLMODE, cull);
	// Stage 3 as drawTrapezoidWater leaves it: with a shroud, unbound through the wrapper (its D3D-texture cache and the device
	// agree); without one, the white texture stays bound, as setupFlatWaterShader leaves it.
	if (shroud)
	{	// as drawTrapezoidWater restores after its own shroud stage
		W3DShaderManager::resetShader(W3DShaderManager::ST_SHROUD_TEXTURE);
		DX8Wrapper::Set_DX8_Texture(3, nullptr);
		GxRawRS(dev, D3DRS_ZFUNC, D3DCMP_EQUAL);
	}
}

// ---------------------------------------------------------------------------------------------------------------------------
// Phase 3, rivers: drawRiverWater's colour and surface, and the river sheen pass (renderWater, right after drawRiverWater).

// drawRiverWater's vertex colour: the deep colour lit by the map, as the standing water's (gxWaterRA3WaterDiffuse), x riverShade
// (phase 3b), alpha kept; the shades are the same bytes, so getRiverVertexDiffuse scales them by the shroud exactly as it scales
// retail's. Off: untouched.
static void gxWaterRA3RiverColour(Real *shadeR, Real *shadeG, Real *shadeB, Int *diffuse)
{
	if (!gxWaterRA3Active() || !gxWaterRA3RiverOn(s_gxRA3Tune))
		return;
	const unsigned ra3 = gxWaterRA3RiverShade(gxWaterRA3WaterDiffuse((unsigned)*diffuse), s_gxRA3Tune);
	*diffuse = (Int)ra3;
	*shadeR = (Real)((ra3 >> 16) & 0xff);
	*shadeG = (Real)((ra3 >> 8) & 0xff);
	*shadeB = (Real)(ra3 & 0xff);
}

// drawRiverWater's stage 0: the smooth surface with the river's own opacity as its alpha (retail's river pass blends by the
// texture's alpha: TWWater01's 194), while the option, rivers and flat are on; otherwise (or if it could not be made) retail's.
static TextureClass *gxWaterRA3RiverSurfaceTexture(TextureClass *retail)
{
	if (!gxWaterRA3Active() || !gxWaterRA3RiverOn(s_gxRA3Tune) || s_gxRA3Tune.flat < 0.5f)
		return retail;
	if (s_gxRA3RiverSurface == nullptr)
	{
		s_gxRA3RiverSurface = MSGNEW("TextureClass") TextureClass(128, 128, WW3D_FORMAT_A8R8G8B8, MIP_LEVELS_ALL);
		s_gxRA3RiverSurface->Get_Filter().Set_U_Addr_Mode(TextureFilterClass::TEXTURE_ADDRESS_REPEAT);
		s_gxRA3RiverSurface->Get_Filter().Set_V_Addr_Mode(TextureFilterClass::TEXTURE_ADDRESS_REPEAT);
		s_gxRA3RiverSurface->Get_Filter().Set_Mag_Filter(TextureFilterClass::FILTER_TYPE_BEST);
		s_gxRA3RiverSurface->Get_Filter().Set_Min_Filter(TextureFilterClass::FILTER_TYPE_BEST);
		s_gxRA3RiverSurface->Get_Filter().Set_Mip_Mapping(TextureFilterClass::FILTER_TYPE_BEST);
		s_gxRA3RiverSurfaceFilled = FALSE;
	}
	if (!s_gxRA3RiverSurfaceFilled || s_gxRA3RiverOpacity != s_gxRA3Tune.riverOpacity)
	{
		enum { N = 128 };
		unsigned *level = new unsigned[ N * N ];
		gxWaterRA3FillSurface(level, N, s_gxRA3Tune.riverOpacity);
		Bool ok = FALSE;
		gxWaterRA3UploadMips(s_gxRA3RiverSurface, level, N, &ok);
		delete [] level;
		s_gxRA3RiverSurfaceFilled = ok;
		s_gxRA3RiverOpacity = s_gxRA3Tune.riverOpacity;
	}
	return s_gxRA3RiverSurfaceFilled ? s_gxRA3RiverSurface : retail;
}

// The river drawRiverWater just drew, once more, ADDED, from buffers of its own (drawRiverWater's dynamic buffers are released by
// now - the shared dynamic buffer moves its offset only when an access ends; the standing pass's). The rows are
// walked exactly as drawRiverWater walks them, and nothing is drawn where it draws nothing. `stage0` / `stage1` are the textures
// drawRiverWater left there (the river surface or retail's, the sparkles setupJbaWaterShader binds raw).
static void gxWaterRA3DrawRiverSheen(PolygonTrigger *pTrig, TextureClass *stage0, TextureClass *stage1)
{
	if (!gxWaterRA3Active() || !gxWaterRA3RiverOn(s_gxRA3Tune) || !gxWaterRA3SheenOn(gxWaterRA3RiverSheenTune(s_gxRA3Tune)) || pTrig == nullptr
		|| stage0 == nullptr || stage1 == nullptr)
		return;
	if (W3DShaderManager::getChipset() < DC_GENERIC_PIXEL_SHADER_1_1 || ShaderClass::Is_Backface_Culling_Inverted())
		return;	// no ps.1.1 (no platform we ship), or a mirror pass
	IDirect3DDevice8 *dev = DX8Wrapper::_Get_D3D_Device8();
	if (dev == nullptr)
		return;

	// drawRiverWater's own guards: the same rows or none.
	const Int numPoints = pTrig->getNumPoints();
	const Int rectangleCount = numPoints / 2 - 1;
	if ((rectangleCount + 1) * 2 * 3 > 65535)
		return;
	Int innerNdx = pTrig->getRiverStart();
	if (innerNdx < 0 || innerNdx >= numPoints - 1)
		return;
	const Int rows = numPoints / 2;
	if (!gxWaterRA3RiverFits(rows))
		return;

	if (s_gxRA3RiverShader == 0)
	{
		if (s_gxRA3RiverShaderFailed)
			return;
		if (FAILED(dev->CreatePixelShader((const DWORD *)kGxWaterRA3RiverSheenShader, &s_gxRA3RiverShader)))
		{
			s_gxRA3RiverShader = 0;
			s_gxRA3RiverShaderFailed = TRUE;
			fprintf(stderr, "[GXWATER] ra3 river sheen: CreatePixelShader FAILED - no motion on this map's rivers\n");
			return;
		}
	}
	gxWaterRA3MakeNormalTexture();
	if (s_gxRA3Normal == nullptr)
		return;
	if (!s_gxRA3NormalFilled || s_gxRA3NormalBump != s_gxRA3Tune.bump || s_gxRA3NormalCells != (Int)(s_gxRA3Tune.cells + 0.5f))
		gxWaterRA3FillNormalTexture();
	if (!s_gxRA3NormalFilled || s_gxRA3Normal->Peek_D3D_Texture() == nullptr || stage0->Peek_D3D_Texture() == nullptr
		|| stage1->Peek_D3D_Texture() == nullptr)
		return;

	// The rows, as drawRiverWater pairs them: a = the point it calls innerPt (u2 = 1), b = outerPt (u2 = 0).
	if (s_gxRA3RiverCap < rows)
	{
		delete [] s_gxRA3RiverMesh;
		delete [] s_gxRA3RiverBanks;
		s_gxRA3RiverMesh = new GxWaterRA3RiverVertex[ (size_t)gxWaterRA3RiverVertexCount(rows) ];
		s_gxRA3RiverBanks = new float[ (size_t)rows * 2 ][3];
		s_gxRA3RiverCap = rows;
	}
	float (*bankA)[3] = s_gxRA3RiverBanks;
	float (*bankB)[3] = s_gxRA3RiverBanks + rows;
	Int outerNdx = innerNdx + 1;
	for (Int i = 0; i < rows; ++i)
	{
		const ICoord3D a = *pTrig->getPoint(outerNdx);
		const ICoord3D b = *pTrig->getPoint(innerNdx);
		outerNdx++;
		innerNdx--;
		if (innerNdx < 0)
			innerNdx = numPoints - 1;
		if (outerNdx >= numPoints)
			outerNdx = 0;
		bankA[i][0] = (float)a.x;	bankA[i][1] = (float)a.y;	bankA[i][2] = (float)a.z;
		bankB[i][0] = (float)b.x;	bankB[i][1] = (float)b.y;	bankB[i][2] = (float)b.z;
	}
	const Int verts = gxWaterRA3RiverMesh(bankA, bankB, rows, s_gxRA3RiverMesh);
	const Int indices = gxWaterRA3RiverIndexCount(rows);
	if (verts != gxWaterRA3RiverVertexCount(rows))
		return;

	// The camera, the sun and the light, as the standing pass takes them (gxWaterRA3DrawSheen).
	Matrix4x4 curView;
	DX8Wrapper::_Get_DX8_Transform(D3DTS_VIEW, curView);
	D3DXMATRIX inv;
	float det;
	D3DXMatrixInverse(&inv, &det, (D3DXMATRIX *)&curView);
	const GxWaterRA3Vec eye = { inv._41, inv._42, inv._43 };
	GxWaterRA3Vec look = { inv._31, inv._32, inv._33 };
	if (look.z > 0.0f)
		look = GxWaterRA3Vec{ -look.x, -look.y, -look.z };
	const Coord3D &lp = TheGlobalData->m_terrainLightPos[0];
	const GxWaterRA3Vec h = gxWaterRA3HalfVector(GxWaterRA3Vec{ -lp.x, -lp.y, -lp.z }, GxWaterRA3Vec{ -look.x, -look.y, -look.z },
		s_gxRA3Tune.tiltMin, s_gxRA3Tune.tiltMax);
	float amb[3], dif[3];
	gxWaterLiveLights(amb, dif);
	float sun[4];
	gxWaterRA3SunConst(dif, s_gxRA3Tune, sun);
	// The tilings in the river's frame (s along, t across), the flow along +s: downstream, the way retail's texture scrolls; the
	// river's own drift and its sky x riverSky (phase 3b) - a copy, the standing pass keeps s_gxRA3Tune.
	const GxWaterRA3Tune riverTune = gxWaterRA3RiverSheenTune(s_gxRA3Tune);
	GxWaterRA3Tiling tiling[2];
	gxWaterRA3Tilings(s_gxLiveClock, 1.0f, 0.0f, riverTune, tiling);
	W3DShroud *shroud = TheTerrainRenderObject ? TheTerrainRenderObject->getShroud() : nullptr;

	DynamicIBAccessClass ib_access(BUFFER_TYPE_DYNAMIC_DX8, (unsigned short)indices);
	{
		DynamicIBAccessClass::WriteLockClass lockib(&ib_access);
		UnsignedShort *curIb = lockib.Get_Index_Array();
		if (curIb == nullptr || gxWaterRA3RiverIndices(rows, curIb) != indices)
			return;
	}
	DynamicVBAccessClass vb_access(BUFFER_TYPE_DYNAMIC_DX8, dynamic_fvf_type, (unsigned short)verts);
	{
		DynamicVBAccessClass::WriteLockClass lock(&vb_access);
		VertexFormatXYZNDUV2 *vb = lock.Get_Formatted_Vertex_Array();
		if (vb == nullptr)
			return;
		for (Int k = 0; k < verts; ++k, ++vb)
		{
			const GxWaterRA3RiverVertex &m = s_gxRA3RiverMesh[k];
			vb->x = m.x;
			vb->y = m.y;
			vb->z = m.z;
			vb->nx = 0.0f;
			vb->ny = 0.0f;
			vb->nz = 1.0f;
			Real shroudScale = 1.0f;	// getRiverVertexDiffuse's cell arithmetic (W3DWater.cpp)
			if (shroud)
				shroudScale = (Real)shroud->getShroudLevel((Int)(m.x / shroud->getCellWidth()), (Int)(m.y / shroud->getCellHeight())) / 255.0f;
			vb->diffuse = gxWaterRA3RiverDiffuse(gxWaterRA3SkyDiffuse(amb, dif, riverTune, gxWaterRA3Fresnel(eye, m.x, m.y, m.z)), m.fade,
				shroudScale);
			gxWaterRA3TilingUV(tiling[0], m.s, m.t, &vb->u1, &vb->v1);
			gxWaterRA3TilingUV(tiling[1], m.s, m.t, &vb->u2, &vb->v2);
		}
	}

	if (!s_gxRA3SaidRiver)
	{
		s_gxRA3SaidRiver = TRUE;
		fprintf(stderr, "[GXWATER] ra3 river sheen shader=0x%x rows=%d verts=%d length=%.0f shroud=%d speed=%.1f\n", (unsigned)s_gxRA3RiverShader,
			(int)rows, (int)verts, s_gxRA3RiverMesh[4 * rows - 1].s, shroud ? 1 : 0, riverTune.speed);
	}

	// Save what this pass changes, from the DEVICE (the wrapper's caches are left as they are).
	DX8Wrapper::Set_Transform(D3DTS_WORLD, Matrix3D(true));
	GxRA3SavedTSS saved[] = {
		{ 0, D3DTSS_TEXCOORDINDEX, 0 }, { 0, D3DTSS_TEXTURETRANSFORMFLAGS, 0 }, { 0, D3DTSS_ADDRESSU, 0 }, { 0, D3DTSS_ADDRESSV, 0 },
		{ 0, D3DTSS_MINFILTER, 0 }, { 0, D3DTSS_MAGFILTER, 0 }, { 0, D3DTSS_MIPFILTER, 0 },
		{ 1, D3DTSS_TEXCOORDINDEX, 0 }, { 1, D3DTSS_TEXTURETRANSFORMFLAGS, 0 }, { 1, D3DTSS_ADDRESSU, 0 }, { 1, D3DTSS_ADDRESSV, 0 },
		{ 1, D3DTSS_MINFILTER, 0 }, { 1, D3DTSS_MAGFILTER, 0 }, { 1, D3DTSS_MIPFILTER, 0 },
		{ 0, D3DTSS_MAXANISOTROPY, 0 }, { 1, D3DTSS_MAXANISOTROPY, 0 } };
	for (GxRA3SavedTSS &v : saved)
		dev->GetTextureStageState(v.stage, v.type, &v.value);
	DWORD srcBlend = D3DBLEND_SRCALPHA, destBlend = D3DBLEND_INVSRCALPHA, fogColour = 0, blendOn = TRUE, cull = D3DCULL_NONE, zWrite = FALSE;
	dev->GetRenderState(D3DRS_SRCBLEND, &srcBlend);
	dev->GetRenderState(D3DRS_DESTBLEND, &destBlend);
	dev->GetRenderState(D3DRS_FOGCOLOR, &fogColour);
	dev->GetRenderState(D3DRS_ALPHABLENDENABLE, &blendOn);
	dev->GetRenderState(D3DRS_CULLMODE, &cull);
	dev->GetRenderState(D3DRS_ZWRITEENABLE, &zWrite);

	for (DWORD st = 0; st < 2; ++st)
	{
		dev->SetTexture(st, s_gxRA3Normal->Peek_D3D_Texture());
		GxRawTSS(dev, st, D3DTSS_TEXCOORDINDEX, st);
		GxRawTSS(dev, st, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
		GxRawTSS(dev, st, D3DTSS_ADDRESSU, D3DTADDRESS_WRAP);
		GxRawTSS(dev, st, D3DTSS_ADDRESSV, D3DTADDRESS_WRAP);
		GxRawTSS(dev, st, D3DTSS_MINFILTER, D3DTEXF_ANISOTROPIC);
		GxRawTSS(dev, st, D3DTSS_MAGFILTER, D3DTEXF_LINEAR);
		GxRawTSS(dev, st, D3DTSS_MIPFILTER, D3DTEXF_LINEAR);
		GxRawTSS(dev, st, D3DTSS_MAXANISOTROPY, 8);
	}
	dev->SetPixelShaderConstant(0, D3DXVECTOR4(h.x, h.y, h.z, 0.0f), 1);
	dev->SetPixelShaderConstant(1, D3DXVECTOR4(sun[0], sun[1], sun[2], sun[3]), 1);
	GxRawRS(dev, D3DRS_ALPHABLENDENABLE, TRUE);
	GxRawRS(dev, D3DRS_SRCBLEND, D3DBLEND_ONE);
	GxRawRS(dev, D3DRS_DESTBLEND, D3DBLEND_ONE);
	GxRawRS(dev, D3DRS_FOGCOLOR, 0);	// an added term fogs toward black
	GxRawRS(dev, D3DRS_CULLMODE, D3DCULL_NONE);	// as drawRiverWater draws the river
	GxRawRS(dev, D3DRS_ZWRITEENABLE, FALSE);	// an added layer on the river's own surface
	dev->SetPixelShader(s_gxRA3RiverShader);
	DX8Wrapper::Set_Index_Buffer(ib_access, 0);
	DX8Wrapper::Set_Vertex_Buffer(vb_access);
	DX8Wrapper::Draw_Triangles(0, (unsigned short)(indices / 3), 0, (unsigned short)verts);
	++s_gxRA3RiverDraws;

	// Back as drawRiverWater left it.
	dev->SetPixelShader(0);
	dev->SetTexture(0, stage0->Peek_D3D_Texture());
	dev->SetTexture(1, stage1->Peek_D3D_Texture());
	for (const GxRA3SavedTSS &v : saved)
		GxRawTSS(dev, v.stage, v.type, v.value);
	GxRawRS(dev, D3DRS_SRCBLEND, srcBlend);
	GxRawRS(dev, D3DRS_DESTBLEND, destBlend);
	GxRawRS(dev, D3DRS_FOGCOLOR, fogColour);
	GxRawRS(dev, D3DRS_ALPHABLENDENABLE, blendOn);
	GxRawRS(dev, D3DRS_CULLMODE, cull);
	GxRawRS(dev, D3DRS_ZWRITEENABLE, zWrite);
}
