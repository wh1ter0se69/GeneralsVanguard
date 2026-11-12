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
 *                     $Archive:: /Commando/Code/ww3d2/render2dsentence.cpp                   $*
 *                                                                                             *
 *                       $Author:: Patrick                  $*
 *                                                                                             *
 *								$Modtime:: 8/29/01 11:16a                                             $*
 *                                                                                             *
 *                    $Revision:: 13                                                          $*
 *                                                                                             *
 *---------------------------------------------------------------------------------------------*
 * Functions:                                                                                  *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#include "render2dsentence.h"
#include "surfaceclass.h"
#include "texture.h"
#include "WWDebug/wwprofile.h"
#include "WWDebug/wwmemlog.h"
#include "dx8wrapper.h"
#include "gxtextcontrast.h"
#include "gxtextsmooth.h"
#include <cstdlib>

// GeneralsX: GX_TEXT_CONTRAST=<k> (prototype, default OFF = the
// rasteriser's coverage unchanged): gxtextcontrast.h's curve applied to the glyph coverage before it becomes the atlas
// alpha. Applied at atlas-bake time, so it costs nothing per frame. Read once (glyphs are baked on the main thread).
static uint8 Gx_Text_Coverage (uint8 coverage)
{
	static uint8 s_lut[256];
	static int s_ready = 0;
	if (!s_ready) {
		const char *env = getenv("GX_TEXT_CONTRAST");
		const float k = (env != nullptr) ? (float)atof(env) : 0.0f;
		for (int i = 0; i < 256; ++i)
			s_lut[i] = gxTextContrast(k, (unsigned char)i);
		s_ready = 1;
	}
	return s_lut[coverage];
}


////////////////////////////////////////////////////////////////////////////////////
//	Local constants
////////////////////////////////////////////////////////////////////////////////////
#define no_TEST_PLACEMENT 1	 // Shows alignment markers for text.

#define TEXTURE_OFFSET 2
////////////////////////////////////////////////////////////////////////////////////
//
//	Render2DSentenceClass
//
////////////////////////////////////////////////////////////////////////////////////
Render2DSentenceClass::Render2DSentenceClass () :
	Font (nullptr),
	Location (0.0F,0.0F),
	Cursor (0.0F,0.0F),
	TextureOffset (0, 0),
	TextureStartX (0),
	CurSurface (nullptr),
	CurrTextureSize (0),
	MonoSpaced (false),
	IsClippedEnabled (false),
	ClipRect (0, 0, 0, 0),
	BaseLocation (0, 0),
	LockedPtr (nullptr),
	LockedStride (0),
	TextureSizeHint (0),
	WrapWidth (0),
	Centered (false),
	DrawExtents (0, 0, 0, 0),
	ParseHotKey( false ),
	useHardWordWrap( false)
{
	Shader = Render2DClass::Get_Default_Shader ();
}


////////////////////////////////////////////////////////////////////////////////////
//
//	~Render2DSentenceClass
//
////////////////////////////////////////////////////////////////////////////////////
Render2DSentenceClass::~Render2DSentenceClass ()
{
	REF_PTR_RELEASE (Font);
	Reset ();
}


////////////////////////////////////////////////////////////////////////////////////
//
//	Set_Font
//
////////////////////////////////////////////////////////////////////////////////////
void
Render2DSentenceClass::Set_Font (FontCharsClass *font)
{
	Reset ();
	REF_PTR_SET (Font, font);
}


////////////////////////////////////////////////////////////////////////////////////
//
//	Reset_Polys
//
////////////////////////////////////////////////////////////////////////////////////
void
Render2DSentenceClass::Reset_Polys ()
{
	for (int index = 0; index < Renderers.Count (); index ++) {
		Renderers[index].Renderer->Reset ();
	}
}


////////////////////////////////////////////////////////////////////////////////////
//
//	Reset
//
////////////////////////////////////////////////////////////////////////////////////
void
Render2DSentenceClass::Reset ()
{
	//
	//	Make sure we unlock the current surface (if necessary)
	//
	if (LockedPtr != nullptr) {
		CurSurface->Unlock ();
		LockedPtr = nullptr;
	}

	//
	//	Release our hold on the current surface
	//
	REF_PTR_RELEASE (CurSurface);

	//
	//	Free each renderer
	//
	while (Renderers.Count () > 0) {
		delete Renderers[0].Renderer;
		Renderers.Delete(0);
	}

	Cursor.Set (0, 0);
	MonoSpaced = false;
	ParseHotKey = false;

	Release_Pending_Surfaces ();
	Reset_Sentence_Data ();
}


////////////////////////////////////////////////////////////////////////////////////
//
//	Make_Additive
//
////////////////////////////////////////////////////////////////////////////////////
void
Render2DSentenceClass::Make_Additive ()
{
	Shader.Set_Dst_Blend_Func (ShaderClass::DSTBLEND_ONE);
	Shader.Set_Src_Blend_Func (ShaderClass::SRCBLEND_ONE);
	Shader.Set_Primary_Gradient (ShaderClass::GRADIENT_MODULATE);
	Shader.Set_Secondary_Gradient (ShaderClass::SECONDARY_GRADIENT_DISABLE);

	Set_Shader (Shader);
}


////////////////////////////////////////////////////////////////////////////////////
//
//	Make_Additive
//
////////////////////////////////////////////////////////////////////////////////////
void
Render2DSentenceClass::Set_Shader (ShaderClass shader)
{
	Shader = shader;

	//
	//	Change each renderer's shader
	//
	for (int i = 0; i < Renderers.Count (); i ++) {
		ShaderClass *curr_shader = Renderers[i].Renderer->Get_Shader ();
		(*curr_shader) = Shader;
	}
}


////////////////////////////////////////////////////////////////////////////////////
//
//	Render
//
////////////////////////////////////////////////////////////////////////////////////
void
Render2DSentenceClass::Render ()
{
	//
	//	Build any textures that are pending
	//
	Build_Textures ();

	//
	//	Ask each renderer to draw its contents
	//
	for (int i = 0; i < Renderers.Count (); i ++) {
		Renderers[i].Renderer->Render ();
	}
}


////////////////////////////////////////////////////////////////////////////////////
//
//	Set_Base_Location
//
////////////////////////////////////////////////////////////////////////////////////
void
Render2DSentenceClass::Set_Base_Location (const Vector2 &loc)
{
	Vector2 dif		= loc - BaseLocation;
	BaseLocation	= loc;
	for (int i = 0; i < Renderers.Count (); i ++) {
		Renderers[i].Renderer->Move (dif);
	}
}


////////////////////////////////////////////////////////////////////////////////////
//
//	Set_Location
//
////////////////////////////////////////////////////////////////////////////////////
void
Render2DSentenceClass::Set_Location (const Vector2 &loc)
{
	Location	= loc;
}


////////////////////////////////////////////////////////////////////////////////////
//
//	Get_Text_Extents
//
////////////////////////////////////////////////////////////////////////////////////
Vector2
Render2DSentenceClass::Get_Text_Extents (const WCHAR *text)
{
	Vector2 extent (0, Font->Get_Char_Height());

	while (*text) {
		WCHAR ch = *text++;

		if ( ch != (WCHAR)'\n' ) {
			extent.X += Font->Get_Char_Spacing( ch );
		}
	}

	return extent;
}


////////////////////////////////////////////////////////////////////////////////////
//
//	Get_Formatted_Text_Extents
//
////////////////////////////////////////////////////////////////////////////////////
Vector2
Render2DSentenceClass::Get_Formatted_Text_Extents (const WCHAR *text)
{
	return Build_Sentence_Not_Centered(text, nullptr, nullptr, true);
}


////////////////////////////////////////////////////////////////////////////////////
//
//	Reset_Sentence_Data
//
////////////////////////////////////////////////////////////////////////////////////
void
Render2DSentenceClass::Reset_Sentence_Data ()
{
	//
	//	Release our hold on each texture used in the sentence
	//
	for (int index = 0; index < SentenceData.Count (); index ++) {
		REF_PTR_RELEASE (SentenceData[index].Surface);
	}

	if (SentenceData.Count()>0) {
		SentenceData.Delete_All ();
	}
}


////////////////////////////////////////////////////////////////////////////////////
//
//	Release_Pending_Surfaces
//
////////////////////////////////////////////////////////////////////////////////////
void
Render2DSentenceClass::Release_Pending_Surfaces ()
{
	//
	//	Release our hold on each pending surface
	//
	for (int index = 0; index < PendingSurfaces.Count (); index ++) {
		SurfaceClass *curr_surface = PendingSurfaces[index].Surface;
		REF_PTR_RELEASE (curr_surface);
	}

	if (PendingSurfaces.Count()>0) PendingSurfaces.Delete_All ();
}


////////////////////////////////////////////////////////////////////////////////////
//
//	Build_Textures
//
////////////////////////////////////////////////////////////////////////////////////
void
Render2DSentenceClass::Build_Textures ()
{
	WWMEMLOG(MEM_TEXTURE);

	//
	//	Make sure we unlock the current surface
	//
	if (LockedPtr != nullptr) {
		CurSurface->Unlock ();
		LockedPtr = nullptr;
	}

	//
	//	Release our hold on the current surface
	//
	REF_PTR_RELEASE (CurSurface);
	TextureOffset.Set (0, 0);
	TextureStartX = 0;

	//
	//	Convert all pending surfaces to textures
	//
	for (int index = 0; index < PendingSurfaces.Count (); index ++) {
		PendingSurfaceStruct &surface_info = PendingSurfaces[index];
		SurfaceClass *curr_surface = surface_info.Surface;

		//
		//	Get the dimensions of the surface
		//
		SurfaceClass::SurfaceDescription desc;
		curr_surface->Get_Description (desc);

		//
		//	Create the new texture
		//
		TextureClass *new_texture = W3DNEW TextureClass (desc.Width, desc.Width, WW3D_FORMAT_A8R8G8B8, MIP_LEVELS_1);
		SurfaceClass *texture_surface = new_texture->Get_Surface_Level ();

		// GeneralsX: Texture creation can fail (out of VRAM, lost device, MoltenVK/DXVK allocation failure), leaving the texture
		// without a D3D surface. Skip this batch instead of dereferencing null; the text simply does not draw. Idea from Generals Online
		// c3c30f33f / 7de4aabaf, reimplemented.
		if (texture_surface == nullptr || curr_surface->Peek_D3D_Surface () == nullptr || texture_surface->Peek_D3D_Surface () == nullptr) {
			fprintf (stderr, "[GeneralsX] Render2DSentence::Build_Textures: no texture surface (%ux%u), text batch skipped\n", (unsigned)desc.Width, (unsigned)desc.Width);
			// The renderers keep their glyph quads, which would draw untextured: empty them so nothing is submitted for this batch.
			for (int renderer_index = 0; renderer_index < surface_info.Renderers.Count (); renderer_index ++) {
				surface_info.Renderers[renderer_index]->Reset ();
			}
			REF_PTR_RELEASE (texture_surface);
			REF_PTR_RELEASE (new_texture);
			REF_PTR_RELEASE (curr_surface);
			continue;
		}

		new_texture->Get_Filter().Set_U_Addr_Mode(TextureFilterClass::TEXTURE_ADDRESS_CLAMP);
		new_texture->Get_Filter().Set_V_Addr_Mode(TextureFilterClass::TEXTURE_ADDRESS_CLAMP);
		new_texture->Get_Filter().Set_Min_Filter(TextureFilterClass::FILTER_TYPE_NONE);
		new_texture->Get_Filter().Set_Mag_Filter(TextureFilterClass::FILTER_TYPE_NONE);
		new_texture->Get_Filter().Set_Mip_Mapping(TextureFilterClass::FILTER_TYPE_NONE);

		//
		//	Copy the contents of the texture from the surface
		//
		DX8Wrapper::_Copy_DX8_Rects (curr_surface->Peek_D3D_Surface (), nullptr, 0, texture_surface->Peek_D3D_Surface (), nullptr);
		REF_PTR_RELEASE (texture_surface);

		//
		//	Assign this texture to any renderers that need it
		//
		for (int renderer_index = 0; renderer_index < surface_info.Renderers.Count (); renderer_index ++) {
			Render2DClass *renderer = surface_info.Renderers[renderer_index];
			renderer->Set_Texture (new_texture);
		}

		//
		//	Release our hold on the objects
		//
		REF_PTR_RELEASE (new_texture);
		REF_PTR_RELEASE (curr_surface);
	}

	//
	//	Reset the list
	//
	if (PendingSurfaces.Count()>0) {
		PendingSurfaces.Delete_All ();
	}
}


////////////////////////////////////////////////////////////////////////////////////
//
//	Draw_Sentence
//
////////////////////////////////////////////////////////////////////////////////////
void
Render2DSentenceClass::Draw_Sentence (uint32 color)
{
	Render2DClass *curr_renderer	= nullptr;
	SurfaceClass *curr_surface		= nullptr;

	DrawExtents.Set (0, 0, 0, 0);

	int offset = 0;
	//
	//	Loop over all the parts of the sentence
	//
	for (int index = 0; index < SentenceData.Count (); index ++) {
		SentenceDataStruct &data = SentenceData[index];

		//
		//	Has the surface changed?
		//
		if (data.Surface != curr_surface) {
			curr_surface = data.Surface;

			//
			//	Try to find a renderer that uses the same "texture"
			//
			bool found = false;
			for (int renderer_index = 0; renderer_index < Renderers.Count (); renderer_index ++) {
				if (Renderers[renderer_index].Surface == curr_surface) {
					found = true;
					curr_renderer = Renderers[renderer_index].Renderer;
					break;
				}
			}

			//
			//	Create a new renderer if we couldn't find an appropriate one
			//
			if (found == false) {

				//
				//	Allocate a new renderer
				//
				curr_renderer = W3DNEW Render2DClass;
				curr_renderer->Set_Coordinate_Range (Render2DClass::Get_Screen_Resolution ());
				ShaderClass *curr_shader = curr_renderer->Get_Shader ();
				(*curr_shader) = Shader;

				//
				//	Add it to our list
				//
				RendererDataStruct render_info;
				render_info.Renderer	= curr_renderer;
				render_info.Surface	= curr_surface;
				Renderers.Add (render_info);

				//
				//	Now, add this renderer to the surface pending list
				//
				for (int surface_index = 0; surface_index < PendingSurfaces.Count (); surface_index ++) {
					PendingSurfaceStruct &surface_info = PendingSurfaces[surface_index];
					if (surface_info.Surface == curr_surface) {
						surface_info.Renderers.Add (curr_renderer);
					}
				}
			}
		}

		//
		//	Get the dimensions of the surface
		//
		SurfaceClass::SurfaceDescription desc;
		curr_surface->Get_Description (desc);

		//
		//	Add a quad that contains this sentence chunk
		//
		RectClass screen_rect	= data.ScreenRect;
		screen_rect					+= Location;
		RectClass uv_rect			= data.UVRect;

		//
		//	Clip the quad (as necessary)
		//
		bool add_quad = true;
		if (IsClippedEnabled) {

			//
			//	Check for completely clipped
			//
			if (	screen_rect.Right <= ClipRect.Left ||
					screen_rect.Bottom <= ClipRect.Top)
			{
				add_quad = false;
			} else {

				//
				//	Clip the polygons to the specified area
				//
				RectClass clipped_rect;
				clipped_rect.Left		= max (screen_rect.Left, ClipRect.Left);
				clipped_rect.Right	= min (screen_rect.Right, ClipRect.Right);
				clipped_rect.Top		= max (screen_rect.Top, ClipRect.Top);
				clipped_rect.Bottom	= min (screen_rect.Bottom, ClipRect.Bottom);

				//
				//	Clip the texture to the specified area
				//
				RectClass clipped_uv_rect;
				float percent				= ((clipped_rect.Left - screen_rect.Left) / screen_rect.Width ());
				clipped_uv_rect.Left		= uv_rect.Left + (uv_rect.Width () * percent);

				percent						= ((clipped_rect.Right - screen_rect.Left) / screen_rect.Width ());
				clipped_uv_rect.Right	= uv_rect.Left + (uv_rect.Width () * percent);

				percent						= ((clipped_rect.Top - screen_rect.Top) / screen_rect.Height ());
				clipped_uv_rect.Top		= uv_rect.Top + (uv_rect.Height () * percent);

				percent						= ((clipped_rect.Bottom - screen_rect.Top) / screen_rect.Height ());
				clipped_uv_rect.Bottom	= uv_rect.Top + (uv_rect.Height () * percent);

				//
				//	Use the clipped rectangles to render
				//
				screen_rect = clipped_rect;
				uv_rect		= clipped_uv_rect;

				if (screen_rect.Right <= screen_rect.Left ||
						screen_rect.Bottom <= screen_rect.Top)
				{
					add_quad = false;
				}
			}
		}

		if (add_quad) {
			//uv_rect.Bottom += 0.5f;
			uv_rect *=  1.0F / ((float)desc.Width);
#ifdef TEST_PLACEMENT
			screen_rect.Left += offset*3;
			screen_rect.Right += offset*3;
#endif
			offset++;
			curr_renderer->Add_Quad (screen_rect, uv_rect, color);

			//
			//	Add this rectangle to the total draw extents
			//
			if (DrawExtents.Width () == 0) {
				DrawExtents = screen_rect;
			} else {
				DrawExtents += screen_rect;
			}
		}
	}
}


////////////////////////////////////////////////////////////////////////////////////
//
//	Record_Sentence_Chunk
//
////////////////////////////////////////////////////////////////////////////////////
void
Render2DSentenceClass::Record_Sentence_Chunk ()
{
	//
	//	Do we have anything to store?
	//
	int width = TextureOffset.I - TextureStartX;
	if (width > 0) {
		float char_height = Font->Get_Char_Height ();

		//
		//	Build a structure that contains enough information
		// to hold this portion of the sentence
		//
		SentenceDataStruct sentence_data;
		sentence_data.Surface = CurSurface;
		sentence_data.Surface->Add_Ref ();
		sentence_data.ScreenRect.Left		= Cursor.X;
		sentence_data.ScreenRect.Right	= Cursor.X + width;
		sentence_data.ScreenRect.Top		= Cursor.Y;
		sentence_data.ScreenRect.Bottom	= Cursor.Y + char_height;
		sentence_data.UVRect.Left			= TextureStartX;
		sentence_data.UVRect.Top			= TextureOffset.J;
		sentence_data.UVRect.Right			= TextureOffset.I;
		sentence_data.UVRect.Bottom		= TextureOffset.J + char_height;

		//
		//	Add this information to our list
		//
		SentenceData.Add (sentence_data);
	}
}


////////////////////////////////////////////////////////////////////////////////////
//
//	Allocate_New_Surface
//
////////////////////////////////////////////////////////////////////////////////////
void
Render2DSentenceClass::Allocate_New_Surface (const WCHAR *text, bool justCalcExtents)
{
	if (!justCalcExtents)
	{
		//
		//	Unlock the last surface (if necessary)
		//
		if (LockedPtr != nullptr) {
			CurSurface->Unlock ();
			LockedPtr = nullptr;
		}
	}

	//
	// Calculate the width of the text and of its widest glyph
	//
	int text_width = 0;
	int max_char_width = 0;
	for (int index = 0; text[index] != 0; index ++) {
		text_width += Font->Get_Char_Spacing (text[index]);
		const int char_width = Font->Get_Char_Width (text[index]);
		max_char_width = max (max_char_width, char_width);
	}

	int char_height = Font->Get_Char_Height ();

	//
	//	TheSuperHackers @fix The texture must hold at least one row of glyphs and the widest glyph
	//	of this text, otherwise the blit runs off the surface. The widest glyph the font can produce
	//	is no use here, because fonts such as Arial report one several times wider than their text.
	//	Fonts that fit search 64 to 256 px, which bounds the texture that every display string
	//	allocates for itself. Larger fonts use the smallest texture that fits.
	//	GeneralsX: Ported by hand from upstream PR #3288 (f7f77dd87, open at
	//	the time). FontLibrary::getFont accepts up to 512 pt since the #3051 port, and from about 170 pt
	//	(Arial) the character height reaches 256, which no 64-256 px texture holds.
	//
	constexpr const int TextureSizeMinPow2 = 6; // 64 px, smallest texture
	constexpr const int TextureSizeSearchMaxPow2 = 8; // 256 px, largest texture searched for fonts that fit
	constexpr const int TextureSizeMaxPow2 = 11; // 2048 px, largest texture for any font

	const int min_extent = max (char_height + 1, max_char_width + TEXTURE_OFFSET + 1);
	int min_pow2 = TextureSizeMinPow2;
	while (min_pow2 < TextureSizeMaxPow2 && (1 << min_pow2) < min_extent) {
		min_pow2 ++;
	}

	int max_pow2 = max (TextureSizeSearchMaxPow2, min_pow2);
	if (max_pow2 > TextureSizeSearchMaxPow2) {
		//
		//	The font does not fit the search range, so use the smallest texture that fits, limited to
		//	the largest texture the device supports.
		//
		const D3DCAPS8 &dx8caps = DX8Wrapper::Get_Current_Caps ()->Get_DX8_Caps ();
		const int max_device_size = (int)min (dx8caps.MaxTextureWidth, dx8caps.MaxTextureHeight);
		while (max_pow2 > TextureSizeMinPow2 && (1 << max_pow2) > max_device_size) {
			max_pow2 --;
		}
		min_pow2 = min (min_pow2, max_pow2);
	}

	//
	//	Find the best texture size for the remaining text
	//
	CurrTextureSize = 1 << min_pow2;
	int best_tex_mem_usage = 999999999;
	for (int pow2 = min_pow2; pow2 <= max_pow2; pow2 ++) {

		int size					= 1 << pow2;
		int row_count			= (text_width / size) + 1;
		int rows_per_texture	= size / (char_height + 1);

		//
		//	Can we even fit one character on this texture?
		//
		if (rows_per_texture > 0) {

			//
			//	How many textures (at this size) would it take to render
			// the remaining text?
			//
			int texture_count	= row_count / rows_per_texture;
			texture_count		= max (texture_count, 1);

			//
			//	Is this the best usage of texture memory we've found yet?
			//
			int texture_mem_usage = (texture_count * size * size);
			if (texture_mem_usage < best_tex_mem_usage) {
				CurrTextureSize		= size;
				best_tex_mem_usage	= texture_mem_usage;
			}
		}
	}

	//
	//	Use whichever is larger, the hint or the calculated size
	//
	CurrTextureSize = max (TextureSizeHint, CurrTextureSize);

	if (!justCalcExtents)
	{
		//
		//	Release our extra hold on the old surface
		//
		REF_PTR_RELEASE (CurSurface);

		//
		//	Create the new surface
		//
// GeneralsX: the glyph atlas is 8-bit alpha, not 4-bit: FreeType and GDI rasterise 8-bit coverage, and 16 levels
// crush the one-pixel stems of small text (13pt list rows). Costs 2x atlas memory (32bpp) for glyph pages only.
		CurSurface = NEW_REF (SurfaceClass, (CurrTextureSize, CurrTextureSize, WW3D_FORMAT_A8R8G8B8));
		WWASSERT (CurSurface != nullptr);
		CurSurface->Add_Ref ();

		//
		//	Add this surface to our list
		//
		PendingSurfaceStruct surface_info;
		surface_info.Surface = CurSurface;
		PendingSurfaces.Add (surface_info);
	}

	//
	//	Reset to the upper left corner
	//
	TextureOffset.Set (0, 0);
	TextureStartX = 0;
}

float FindStartingXPos( const WCHAR *text )
{

	return 1;
}
////////////////////////////////////////////////////////////////////////////////////
//
//	Build_Sentence_Centered
//
////////////////////////////////////////////////////////////////////////////////////
void	Render2DSentenceClass::Build_Sentence_Centered (const WCHAR *text, int *hkX, int *hkY)
{
	float char_height = Font->Get_Char_Height ();
	int notCenteredHotkeyX = 0;
	int notCenteredHotkeyY = 0;
	Vector2 extent = Build_Sentence_Not_Centered(text,&notCenteredHotkeyX, &notCenteredHotkeyY, TRUE); //Get_Formatted_Text_Extents(text);

	//
	//	Start fresh
	//
	Reset_Sentence_Data ();
	Cursor.Set (0, 0);

	//
	//	Ensure we have a surface to start with
	//
	if (CurSurface == nullptr) {
		Allocate_New_Surface (text);
	}



	//
	//	Loop over all the characters in the string
	//
	bool end = false;
	const WCHAR *word;
	int word_width	= 0;
	int line_width	= 0;
	int charCount = 0;
	int wordCount = 0;
	int hotKeyPosX = 0;
	int hotKeyPosY = 0;
	bool calcHotKeyX = false;
	bool dontBlit = false;
	while (!end)
	{
		//
		// Re-init everything for the next line
		//
		word	= text;
		word_width	= 0;
		line_width	= 0;
		charCount = 0;
		wordCount = 0;
		//
		//first find the length of the line till we wrap
		//
		while ( 1 )
		{
			//
			// read a word
			//
			int word_spacing = 0;
			while ((*word != 0) && (*word > L' ') && (*word != L'\n')) {
				// GeneralsX: test the character after the '&', not (*word)+1, which always passes all three guards, so every '&'
				// counted as a hot-key marker ("Load & Save" mis-measured; a trailing '&' stepped onto the terminator).
				// This matches the blitting loop below and Build_Sentence_Not_Centered.
				if( ParseHotKey && (*word == L'&') && (*(word+1) != 0) && (*(word+1) > L' ') && (*(word+1) != L'\n'))
				{
					int offset = 0;
					if (word_width != 0 )
					{
						const WCHAR *word_back = word;
						*word_back--;
						if (*word_back == L' ')
						{
							line_width -= word_width;
							offset =-1;
						}
					}
					word++;
					calcHotKeyX = true;
				}

				word_spacing = Font->Get_Char_Spacing (*word++);
				word_width += word_spacing;
				wordCount++;

				if (WrapWidth > 0 && word_width >= WrapWidth && useHardWordWrap)
					break;
			}
			//
			// If this word is unworthy to be on the current line, decrement the space and break
			//
			if(WrapWidth > 0 && (line_width + word_width >= WrapWidth))
			{
				//
				//Take care of the case that the word is too big for the allocated space...
				//If that's the case, drop out and process the word anyway
				//
				if(charCount == 0)
				{
					charCount +=wordCount - 1;
					line_width += word_width - word_spacing;
					if(*word == 0)
						end = true;
					break;
				}
				charCount--;
				break;
			}
			//
			// if we reached the end of the text, set the values and break, also set the end flag
			//
			if( *word == 0 )
			{
				charCount +=wordCount;
				line_width += word_width;
				end = true;
				break;
			}
			//
			// otherwise, increment the counts
			//
			charCount +=wordCount + 1;
			line_width += word_width;
			//
			// We were some a new line character break and process
			//
			if(*word != L' ')
				break;
			//
			// add the space to our width
			//
			word_width = Font->Get_Char_Spacing (*word++);
			wordCount = 0;
			line_width += word_width;
		}
		//
		// we now hold the length of the line and it's width lets set our cursor position to center it
		//
		Cursor.X = (int)((extent.X - line_width) / 2);
		if(Cursor.X < 0)
			Cursor.X = 0;
		if(calcHotKeyX)
		{
			calcHotKeyX = false;
			hotKeyPosX = Cursor.X + notCenteredHotkeyX;
		}

		for(int i = 0; i <= charCount; i++) {
			// TheSuperHackers @fix The text still to be placed, starting at ch, so a new surface is sized for ch as well.
			const WCHAR *remaining_text = text;
			WCHAR ch = *text++;
			dontBlit = false;
			//
			//	Determine how much horizontal space this character requires
			//
			if(ParseHotKey && (ch == L'&') && (*text != 0) && (*text > L' ') && (*text != L'\n'))
			{
				remaining_text = text;
				ch = *text++;
				dontBlit = true;
			}
			int char_spacing = Font->Get_Char_Spacing (ch);
			const int char_width = Font->Get_Char_Width (ch);

			bool exceeded_texture_width	= ((TextureOffset.I + char_width) >= CurrTextureSize);
			bool encountered_break_char	= (ch == L' ' || ch == L'\n' || ch == 0);

			//
			//	Do we need to record this portion of the sentence to its own chunk?
			//
			if (exceeded_texture_width || encountered_break_char) {
				Record_Sentence_Chunk ();

				//
				//	Adjust the positions
				//
				Cursor.X			+= (TextureOffset.I - TextureStartX);
				TextureStartX	= TextureOffset.I;

				//
				//	Adjust the output coordinates
				//
				if (ch == L' ') {
					Cursor.X += char_spacing;
				} else if ((ch == 0 )|| (ch == L'\n')) {
					break;
				}

				//
				//	Did the text extend past the edge of the texture?
				//
				if (exceeded_texture_width) {
					TextureStartX		= 0;
					TextureOffset.I	= TextureStartX;
					TextureOffset.J	+= char_height;

					//
					//	Did the text extent completely off the texture?
					//
					if ((TextureOffset.J + char_height) >= CurrTextureSize) {
						Allocate_New_Surface (remaining_text);
					}
				}
			}
			//
			//	Adjust the output coordinates
			//
			if (ch != L'\n' && ch != L' ') {

				//
				//	Ensure the surface is locked
				//
				if (LockedPtr == nullptr) {
					LockedPtr = (uint32 *)CurSurface->Lock (&LockedStride);
					WWASSERT (LockedPtr != nullptr);
				}

				//
				//	Check to ensure the text will fit on this texture
				//
				const bool fits_texture = ((TextureOffset.I + char_width) < CurrTextureSize) && ((TextureOffset.J + char_height) < CurrTextureSize);
				WWASSERT (fits_texture);

				//
				//	Blit the character to the surface
				//	TheSuperHackers @fix Skip a glyph that still does not fit.
				//
				if(!dontBlit && fits_texture)
					Font->Blit_Char (ch, LockedPtr, LockedStride, TextureOffset.I, TextureOffset.J, CurrTextureSize, CurrTextureSize);

				if (dontBlit) {
					// we don't blit for a hot key character.  So add extra spacing.
					char_spacing += Font->Get_Extra_Overlap();
					// Brutal hack #27 Gamma - Bolded M's are just a problem.	jba.
					if (ch=='M') {
						char_spacing++;
					}
				}

				TextureOffset.I += char_spacing;
			}
		}
		//
		// reset our cursor and add a line of text to the cursor position
		//
		Cursor.X = 0;
		Cursor.Y += char_height;
		line_width = 0;
		}

		if(hkX)
			*hkX = hotKeyPosX;
		if(hkX)
			*hkY = hotKeyPosY;
}
////////////////////////////////////////////////////////////////////////////////////
//
//	Build_Sentence_NotCentered
//
////////////////////////////////////////////////////////////////////////////////////
Vector2	Render2DSentenceClass::Build_Sentence_Not_Centered (const WCHAR *text, int *hkX, int *hkY, bool justCalcExtents)
{
	Vector2 cursor = Cursor;
	int textureStartX = TextureStartX;
	float maxX = 0;

	int hotKeyPosX = 0;
	int hotKeyPosY = 0;
	bool calcHotKeyX = false;
	bool dontBlit = false;
	Vector2i textureOffset = TextureOffset;
	int currTextureSize = CurrTextureSize;


	//
	//	Start fresh
	//
	if (!justCalcExtents)
	{
		Reset_Sentence_Data ();
	}
	Cursor.Set (0, 0);

	//
	//	Ensure we have a surface to start with
	//
	if (CurSurface == nullptr) {
		Allocate_New_Surface (text, justCalcExtents);
	}

	TextureOffset.Set (TEXTURE_OFFSET, 0);
	TextureStartX = TEXTURE_OFFSET;

	float char_height = Font->Get_Char_Height ();

	//
	//	Loop over all the characters in the string
	//
	while (text != nullptr) {
		// TheSuperHackers @fix The text still to be placed, starting at ch, so a new surface is sized for ch as well.
		const WCHAR *remaining_text = text;
		WCHAR ch = *text++;
		dontBlit = false;
		//
		//	Determine how much horizontal space this character requires
		//
		if(ParseHotKey && (ch == L'&') && (*text != 0) && (*text > L' ') && (*text != L'\n'))
		{
				hotKeyPosY = Cursor.Y;
			if (calcHotKeyX)
				hotKeyPosX = 0;
			else
				hotKeyPosX = Cursor.X + TextureOffset.I -TextureStartX;//TextureOffset.I;

			remaining_text = text;
			ch = *text++;
			dontBlit = true;
		}
		int char_spacing = Font->Get_Char_Spacing (ch);
		const int char_width = Font->Get_Char_Width (ch);

		bool exceeded_texture_width	= ((TextureOffset.I + char_width) >= CurrTextureSize);
		bool encountered_break_char	= (ch == L' ' || ch == L'\n' || ch == 0);
		bool wordBiggerThenLine = ((useHardWordWrap) && ( WrapWidth != 0 ) &&((Cursor.X + TextureOffset.I -TextureStartX + char_spacing) >= WrapWidth));
		//
		//	Do we need to record this portion of the sentence to its own chunk?
		//
		if (exceeded_texture_width || encountered_break_char|| wordBiggerThenLine) {
			if (!justCalcExtents)
			{
				Record_Sentence_Chunk ();
			}

			//
			//	Adjust the positions
			//
			Cursor.X			+= (TextureOffset.I - TextureStartX);
			maxX = max(maxX, Cursor.X);
			TextureStartX	= TextureOffset.I;

			//
			//	Adjust the output coordinates
			//
			if (ch == L' ') {
				//Cursor.X += char_spacing;
				//maxX = max(maxX, Cursor.X);

				//
				// Check to see if we need to wrap on this word-break
				//
				if (WrapWidth > 0) {

					//
					//	Find the length of the next word
					//
					const WCHAR *word	= text;
					float word_width	= char_spacing;
					while ((*word != 0) && (*word > L' ')) {
						// GeneralsX: same (*word)+1 vs *(word+1) mistake as in
						// Build_Sentence_Centered -- the guard was constant-true for every '&',
						// so the marker skip below fired unconditionally. On a string ending in
						// a bare '&' that stepped 'word' onto the terminator, and the
						// Get_Char_Spacing call then consumed the NUL and advanced past it, so
						// this loop kept measuring whatever followed the string in memory.
						// It also mis-measured "&" used as a literal (e.g. "Load & Save"),
						// disagreeing with the blitting pass above that tests the next character.
						if(ParseHotKey && (*word == L'&') && (*(word+1) != 0) && (*(word+1) > L' ') && (*(word+1) != L'\n'))
							word++;
						word_width += Font->Get_Char_Spacing (*word++);
					}

					//
					//	Should we wrap the next word?
					//
					if ((Cursor.X + word_width) >= WrapWidth) {
						Cursor.X = 0;
						Cursor.Y += char_height;
						calcHotKeyX = true;
					}
				}

			} else if (ch == L'\n') {
				Cursor.X = 0;
				Cursor.Y += char_height;
			} else if (ch == 0) {
				break;
			} else if (wordBiggerThenLine){ // we've entered this loop because we're greater then the wordwrap so we need to force a wordwrap
				Cursor.X = 0;
				Cursor.Y += char_height;
			}


			//
			//	Did the text extend past the edge of the texture?
			//
			if (exceeded_texture_width) {
				TextureStartX		= TEXTURE_OFFSET;
				TextureOffset.I	= TextureStartX;
				TextureOffset.J	+= char_height;

				//
				//	Did the text extent completely off the texture?
				//
				if ((TextureOffset.J + char_height) >= CurrTextureSize) {
					Allocate_New_Surface (remaining_text, justCalcExtents);
				}
			}
		}

		if (ch != L'\n' ) {

			//
			//	Ensure the surface is locked
			//
			if (!justCalcExtents)
			{
				if (LockedPtr == nullptr) {
					LockedPtr = (uint32 *)CurSurface->Lock (&LockedStride);
					WWASSERT (LockedPtr != nullptr);
				}
			}

			//
			//	Check to ensure the text will fit on this texture
			//
			const bool fits_texture = ((TextureOffset.I + char_width) < CurrTextureSize) && ((TextureOffset.J + char_height) < CurrTextureSize);
			WWASSERT (fits_texture);

			//
			//	Blit the character to the surface
			//	TheSuperHackers @fix Skip a glyph that still does not fit.
			//
			if (!justCalcExtents && !dontBlit && fits_texture)
			{
				Font->Blit_Char (ch, LockedPtr, LockedStride, TextureOffset.I, TextureOffset.J, CurrTextureSize, CurrTextureSize);
			}
			TextureOffset.I += char_spacing;
		}
	}

	Vector2 extent;
	extent.X = maxX + Font->Get_Extra_Overlap();
	extent.Y = Cursor.Y + char_height;

	Cursor = cursor;
	TextureOffset = textureOffset;
	TextureStartX = textureStartX;

	//
	//	TheSuperHackers @fix Measuring allocates no surface, so it must not leave behind the texture
	//	size of a surface it did not create. The next pass would otherwise check its glyphs against a
	//	size that differs from the surface it actually locks.
	//
	if (justCalcExtents) {
		CurrTextureSize = currTextureSize;
	}

	if(hkX)
		*hkX = hotKeyPosX;
	if(hkX)
		*hkY = hotKeyPosY;

	return extent;
}


////////////////////////////////////////////////////////////////////////////////////
//
//	Build_Sentence
//
////////////////////////////////////////////////////////////////////////////////////
void
Render2DSentenceClass::Build_Sentence (const WCHAR *text, int *hkX, int *hkY)
{
	if (text == nullptr) {
		return ;
	}

	if (Font == nullptr)
		return;

	if(Centered && (WrapWidth > 0 || wcschr(text,L'\n')))
		Build_Sentence_Centered(text, hkX, hkY);
	else
		Build_Sentence_Not_Centered(text, hkX, hkY);

}


////////////////////////////////////////////////////////////////////////////////////
//
//	FontCharsClass
//
////////////////////////////////////////////////////////////////////////////////////
FontCharsClass::FontCharsClass () :
#ifdef _WIN32
	OldGDIFont(	nullptr ),
	OldGDIBitmap( nullptr ),
	GDIFont( nullptr ),
	GDIBitmap( nullptr ),
	GDIBitmapBits ( nullptr ),
	MemDC( nullptr ),
#endif
#if defined(SAGE_USE_FREETYPE) && !defined(_WIN32)
	FTLibrary( nullptr ),
	FTFace( nullptr ),
#endif
	CurrPixelOffset( 0 ),
	PointSize( 0 ),
	CharHeight( 0 ),
	UnicodeCharArray( nullptr ),
	FirstUnicodeChar( 0xFFFF ),
	LastUnicodeChar( 0 ),
	IsBold (false),
	Supersample( 1 )
{
	AlternateUnicodeFont = nullptr;
	::memset( ASCIICharArray, 0, sizeof (ASCIICharArray) );
}


////////////////////////////////////////////////////////////////////////////////////
//
//	~FontCharsClass
//
////////////////////////////////////////////////////////////////////////////////////
FontCharsClass::~FontCharsClass ()
{
	while ( BufferList.Count() ) {
		delete [] BufferList[0].Buffer;
		BufferList.Delete(0);
	}

#if defined(SAGE_USE_FREETYPE) && !defined(_WIN32)
	Free_Freetype_Font();
#else
	Free_GDI_Font();
#endif
	Free_Character_Arrays();
}


////////////////////////////////////////////////////////////////////////////////////
//
//	Get_Char_Data
//
////////////////////////////////////////////////////////////////////////////////////
const FontCharsClassCharDataStruct *
FontCharsClass::Get_Char_Data (WCHAR ch)
{
	// GeneralsX:/felipebraz 14/03/2026 Normalize to 16-bit code units to match legacy font array indexing on non-Windows wchar_t
	const uint16 normalized_char = static_cast<uint16>(ch);
	const WCHAR glyph = static_cast<WCHAR>(normalized_char);

	const FontCharsClassCharDataStruct *retval = nullptr;

	if ( normalized_char < 256 )
	{
		retval = ASCIICharArray[normalized_char];
	}
 	else if ( Get_Char_Font( glyph ) != this )
	{
		return AlternateUnicodeFont->Get_Char_Data( glyph );
	}
	else
	{
		Grow_Unicode_Array( glyph );
		retval = UnicodeCharArray[normalized_char - FirstUnicodeChar];
	}

	//
	//	If the character wasn't found, then add it to our list
	//  TheSuperHackers @feature FreeType port 10/02/2026 Dispatch to FreeType on Linux
	//
	if ( retval == nullptr ) {
#if defined(SAGE_USE_FREETYPE) && !defined(_WIN32)
		retval = Store_Freetype_Char( glyph );
#else
		retval = Store_GDI_Char( glyph );
#endif
	}

	// GeneralsX: Store_*_Char can legitimately fail (a FreeType render error,
	// or a degenerate glyph cell), so guard the assert - it dereferences retval, and
	// a DEBUG_CRASHING build would have crashed here on the null.
	WWASSERT( retval == nullptr || retval->Value == glyph );
	return retval;
}


////////////////////////////////////////////////////////////////////////////////////
//
//	Get_Char_Font
//
////////////////////////////////////////////////////////////////////////////////////
const FontCharsClass *
FontCharsClass::Get_Char_Font (WCHAR ch) const
{
	//
	//	Characters outside the first 256 come from the alternate Unicode font, if there is one
	//	GeneralsX: Tested on the same 16-bit code unit Get_Char_Data indexes by, and
	//	followed down the alternate's own alternate as Get_Char_Data's recursion is (upstream stops at one
	//	level), so this always names the font that stored the glyph Get_Char_Data returns.
	//
	if ( static_cast<uint16>(ch) >= 256 && AlternateUnicodeFont && this != AlternateUnicodeFont ) {
		return AlternateUnicodeFont->Get_Char_Font( ch );
	}

	return this;
}


////////////////////////////////////////////////////////////////////////////////////
//
//	Get_Char_Width
//
////////////////////////////////////////////////////////////////////////////////////
int
FontCharsClass::Get_Char_Width (WCHAR ch)
{
	const FontCharsClassCharDataStruct	* data = Get_Char_Data( ch );
	if ( data != nullptr ) {
		return data->Width;
	}

	return 0;
}


////////////////////////////////////////////////////////////////////////////////////
//
//	Get_Char_Spacing
//
////////////////////////////////////////////////////////////////////////////////////
int
FontCharsClass::Get_Char_Spacing (WCHAR ch)
{
	const FontCharsClassCharDataStruct	* data = Get_Char_Data( ch );
	if ( data != nullptr ) {
		if ( data->Width != 0 ) {
			return data->Width - PixelOverlap - CharOverhang;
		}
	}

	return 0;
}


////////////////////////////////////////////////////////////////////////////////////
//
//	Blit_Char
//
////////////////////////////////////////////////////////////////////////////////////
void
FontCharsClass::Blit_Char (WCHAR ch, uint32 *dest_ptr, int dest_stride, int x, int y, int surface_width, int surface_height)
{
	const FontCharsClassCharDataStruct	* data = Get_Char_Data( ch );
	if ( data != nullptr && data->Width != 0 ) {

		//
		//	Setup the src and destination pointers
		//
		int dest_inc		= (dest_stride >> 2);	// stride is BYTES; 32bpp now, so /4 not /2
		uint32 *src_ptr	= data->Buffer;

// GeneralsX: clip the glyph to the atlas; WWASSERT compiles out without DEBUG_CRASHING, so shipping had no fit check.
// Vertically a tall font can exceed the 256x256 fallback surface (Allocate_New_Surface grows it, callers skip misfits).
// Horizontally the write is data->Width columns but spacing is Width - PixelOverlap - CharOverhang, so it smears.
		// dest_ptr is checked first: a failed Lock() reaches this loop as a null base pointer.
		if ( dest_ptr == nullptr ) {
			return ;
		}

		if ( x < 0 || y < 0 || x >= surface_width || y >= surface_height ) {
			return ;
		}

		int max_rows = surface_height - y;
		if ( max_rows > CharHeight ) {
			max_rows = CharHeight;
		}

		int max_cols = surface_width - x;
		if ( max_cols > data->Width ) {
			max_cols = data->Width;
		}

		dest_ptr				+= (dest_inc * y) + x;

		//
		//	TheSuperHackers @bugfix The glyph holds as many rows as the font that stored it, which is the
		//	alternate Unicode font for characters outside the first 256. That font can be shorter than
		//	this one, for example SimSun is 16 px where Arial is 18 px at 12 point. Read only the rows
		//	the glyph has and clear the remaining rows, instead of reading past the glyph.
		//	GeneralsX: Hand port of upstream #3549 (516471dc3) onto the clipped
		//	32-bit copy above: the glyph's own rows bound the read inside max_rows. The rows it lacks are
		//	written as an empty source row would be - the PixelOverlap columns keep the previous glyph's
		//	edge, as the copy loop's OR keeps them - where upstream clears the whole cell width.
		//
		int glyph_rows = Get_Char_Font( ch )->CharHeight;
		if ( glyph_rows > max_rows ) {
			glyph_rows = max_rows;
		}

		int row;

		//
		//	Simply copy the data from the src buffer to the destination
		//
		for ( row = 0; row < glyph_rows; row ++ ) {
			for ( int col = 0; col < max_cols; col ++ ) {
				uint32 curData = src_ptr[col];
				if (col<PixelOverlap) {
					curData |= dest_ptr[col];
				}
				dest_ptr[col] = curData;
			}
			//
			//	The source cell is data->Width pixels per row whatever we clipped to, so step the
			//	source by the cell width and not by the number of columns actually written.
			//
			src_ptr		+= data->Width;
			dest_ptr	+= dest_inc;
		}

		//
		//	The remaining rows are empty.
		//
		for ( ; row < max_rows; row ++ ) {
			for ( int col = PixelOverlap; col < max_cols; col ++ ) {
				dest_ptr[col] = 0;
			}
			dest_ptr	+= dest_inc;
		}
	}
}


#ifdef _WIN32

////////////////////////////////////////////////////////////////////////////////////
//
//	Store_GDI_Char
//
// GeneralsX @build fbraz 11/02/2026 - Windows-only GDI text rendering
////////////////////////////////////////////////////////////////////////////////////
const FontCharsClassCharDataStruct *
FontCharsClass::Store_GDI_Char (WCHAR ch)
{
	int width	= PointSize * 2;
	int height	= PointSize * 2;

	//
	//	Draw the character into the memory DC
	//
	RECT rect = { 0, 0, width, height };
	int xOrigin = 0;
	if (ch == 'W') {
		xOrigin = 1;
	}
	::ExtTextOutW( MemDC, xOrigin, 0, ETO_OPAQUE, &rect, &ch, 1, nullptr);

	//
	//	Get the size of the character we just drew
	//
	SIZE char_size = { 0 };
	::GetTextExtentPoint32W( MemDC, &ch, 1, &char_size );
	char_size.cx += PixelOverlap + xOrigin;
	//
	//	Get a pointer to the surface that this character should use
	//
	if ( !Update_Current_Buffer( char_size.cx ) ) {
		return nullptr;
	}
	uint32* curr_buffer_p = BufferList[BufferList.Count () - 1].Buffer;
	curr_buffer_p += CurrPixelOffset;

	//
	//	Copy the BMP contents to the buffer
	//
	int stride = (((width * 3) + 3) & ~3);
	// GeneralsX: The glyph's measured extent can exceed the DIB (and the cell Update_Current_Buffer sized): read only
	// rows/columns that exist in the DIB and pad the rest with transparent, so the read stays in bounds and the cell layout is unchanged.
	// Idea from Generals Online c79df3c74, reimplemented.
	int copy_rows = char_size.cy;
	if (copy_rows > height) copy_rows = height;
	if (copy_rows > CharHeight) copy_rows = CharHeight;
	for (int row = 0; row < copy_rows; row ++) {

		//
		//	Compute the indices into the BMP and surface
		//
		int index = (row * stride);

		//
		//	Loop over each column
		//
		for (int col = 0; col < char_size.cx; col ++) {

			//
			//	Get the pixel color at this location
			//
			// ClearType glyph (Create_GDI_Font): the mean of the pixel's three subpixel samples (gxtextsmooth.h)
			uint8 pixel_value = (col < width) ? gxClearTypeCoverage( (const unsigned char *)GDIBitmapBits + index ) : 0;
			index += 3;
#ifdef TEST_PLACEMENT
 			if (row==CharHeight-1&&col==0) {
 				pixel_value = 0xff;
 			}
 			if (row==CharHeight-2&&col==1) {
 				pixel_value = 0xff;
 			}
 			if (row==0&&col==0) {
 				pixel_value = 0xff;
 			}
 			if (row==1&&col==1) {
 				pixel_value = 0xff;
 			}
 			if (row==CharHeight-1&&col==char_size.cx-1-PixelOverlap) {
 				pixel_value = 0xff;
 			}
 			if (row==CharHeight-2&&col==char_size.cx-2-PixelOverlap) {
 				pixel_value = 0xff;
 			}
 			if (row==0&&col==char_size.cx-1-PixelOverlap) {
 				pixel_value = 0xff;
 			}
 			if (row==1&&col==char_size.cx-2-PixelOverlap) {
 				pixel_value = 0xff;
 			}
 			if (pixel_value == 0x00) {
 				pixel_value = 0x40;
 			}
#endif

			//
			//	White RGB with the rasteriser's FULL 8-bit coverage as alpha - no quantisation.
			//
			*curr_buffer_p++ = ((uint32)Gx_Text_Coverage(pixel_value) << 24) | 0x00FFFFFF;
		}
	}

	//
	//	Save information about this character in our list
	//
	FontCharsClassCharDataStruct *char_data	= W3DNEW FontCharsClassCharDataStruct;
	char_data->Value				= ch;
	char_data->Width				= char_size.cx;
	char_data->Buffer				= BufferList[BufferList.Count () - 1].Buffer + CurrPixelOffset;

	//
	//	Insert this character into our array
	//
	if ( ch < 256 ) {
		ASCIICharArray[ch] = char_data;
	} else {
		UnicodeCharArray[ch - FirstUnicodeChar] = char_data;
	}

	//
	//	Advance the character position
	//
	CurrPixelOffset += ((char_size.cx+PixelOverlap) * CharHeight);

	//
	//	Return the index of the entry we just added
	//
	return char_data;
}


////////////////////////////////////////////////////////////////////////////////////
//
//	Create_GDI_Font
//
////////////////////////////////////////////////////////////////////////////////////
bool
FontCharsClass::Create_GDI_Font (const char *font_name)
{
	HDC screen_dc = ::GetDC ((HWND)WW3D::Get_Window());

	const char *fontToUseForGenerals = "Arial";
	bool doingGenerals = false;
	// "GXHUD:<name>" = a GeneralsX panel font: on this path it is only a name (every GDI font is ClearType, below).
	gxTextSmoothName(font_name, &font_name);
	if (strcmp(font_name, "Generals")==0) {
		font_name = fontToUseForGenerals;
		doingGenerals = true;
	}

	//
	//	Calculate the height of the font in logical units
	//
	const int dotsPerInch = 96; // always use 96.	jba.
	int font_height = -MulDiv (PointSize, dotsPerInch, 72);

	int fontWidth = 0; // use font default.
	if (doingGenerals) {
		//fontWidth = -font_height*0.35f; //2 pixels tighter.
		fontWidth = -font_height*0.40f; // one pixel tighter
	}
	PixelOverlap = (-font_height)/8;

	// Sanity check in case of perversion. :)
	if (PixelOverlap<0) PixelOverlap = 0;
	if (PixelOverlap>4) PixelOverlap = 4;
	//
	//	Create the Windows font
	//
	// GeneralsX: CLEARTYPE_QUALITY, not ANTIALIASED_QUALITY: ANTIALIASED_QUALITY obeys the font's 'gasp' table, and Arial's says
	// 9-17 ppem = GRIDFIT only, so GDI drew those sizes bilevel. ClearType smooths them, and CLEARTYPE_QUALITY (not _NATURAL_)
	// keeps GDI's advance widths and metrics, so layout is identical.
	// Store_GDI_Char reads the mean of the three subpixel samples as coverage (gxClearTypeCoverage).
	DWORD bold		= IsBold ? FW_BOLD : FW_NORMAL;
	DWORD italic	= 0;
	GDIFont			= ::CreateFont (font_height, fontWidth, 0, 0, bold, italic,
								FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
								CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
								VARIABLE_PITCH, font_name);

	//
	// Set-up the fields of the BITMAPINFOHEADER
	//	Note: Top-down DIBs use negative height in Win32.
	//
	BITMAPINFOHEADER bitmap_info = { 0 };
	bitmap_info.biSize				= sizeof (BITMAPINFOHEADER);
	bitmap_info.biWidth				= PointSize * 2;
	bitmap_info.biHeight				= -(PointSize * 2);
	bitmap_info.biPlanes				= 1;
	bitmap_info.biBitCount			= 24;
	bitmap_info.biCompression		= BI_RGB;
	bitmap_info.biSizeImage			= ((PointSize * PointSize * 4) * 3);
	bitmap_info.biXPelsPerMeter	= 0;
	bitmap_info.biYPelsPerMeter	= 0;
	bitmap_info.biClrUsed			= 0;
	bitmap_info.biClrImportant		= 0;

	//
	// Create a bitmap that we can access the bits directly of
	//
	GDIBitmap	= ::CreateDIBSection (	screen_dc,
													(const BITMAPINFO *)&bitmap_info,
													DIB_RGB_COLORS,
													(void **)&GDIBitmapBits,
													nullptr,
													0L);

	//
	//	Create a device context we can select the font and bitmap into
	//
	MemDC = ::CreateCompatibleDC (screen_dc);

	//
	// Release our temporary screen DC
	//
	::ReleaseDC ((HWND)WW3D::Get_Window(), screen_dc);

	//
	//	Now select the BMP and font into the DC
	//
	OldGDIBitmap	= (HBITMAP)::SelectObject (MemDC, GDIBitmap);
	OldGDIFont		= (HFONT)::SelectObject (MemDC, GDIFont);
	::SetBkColor (MemDC, RGB (0, 0, 0));
	::SetTextColor (MemDC, RGB (255, 255, 255));

	//
	//	Lookup the pixel height of the font
	//
	TEXTMETRIC text_metric = { 0 };
	::GetTextMetrics (MemDC, &text_metric);
	CharHeight = text_metric.tmHeight;
	CharAscent = text_metric.tmAscent;
	CharOverhang = text_metric.tmOverhang;
	if (doingGenerals) {
		CharOverhang = 0;
	}

	return GDIFont != nullptr && GDIBitmap != nullptr;
}


////////////////////////////////////////////////////////////////////////////////////
//
//	Free_GDI_Font
//
////////////////////////////////////////////////////////////////////////////////////
void
FontCharsClass::Free_GDI_Font ()
{
	//
	//	Select the old font back into the DC and delete
	// our font object
	//
	if ( GDIFont != nullptr ) {
		::SelectObject( MemDC, OldGDIFont );
		::DeleteObject( GDIFont );
		GDIFont = nullptr;
	}

	//
	//	Select the old bitmap back into the DC and delete
	// our bitmap object
	//
	if ( GDIBitmap != nullptr ) {
		::SelectObject( MemDC, OldGDIBitmap );
		::DeleteObject( GDIBitmap );
		GDIBitmap = nullptr;
	}

	//
	//	Delete our memory DC
	//
	if ( MemDC != nullptr ) {
		::DeleteDC( MemDC );
		MemDC = nullptr;
	}
}

#endif // _WIN32

// GeneralsX @build fbraz 11/02/2026 - Used by both Windows GDI and Linux FreeType
// GeneralsX: per upstream PR #3268 the new buffer is max(CHAR_BUFFER_LEN, char_len), so any cell fits and is drawn.
// A degenerate cell is refused: false makes the Store_*_Char callers skip the glyph, so Get_Char_Data returns nullptr,
// which Blit_Char, Get_Char_Width and Get_Char_Spacing already handle.
bool
FontCharsClass::Update_Current_Buffer (int char_width)
{
	if ( char_width <= 0 || CharHeight <= 0 ) {
		return false;
	}

	const int char_len = char_width * CharHeight;

	//
	//	Check to see if we need to allocate a new buffer
	//
	bool needs_new_buffer = (BufferList.Count () == 0);
	if (needs_new_buffer == false) {

		//
		//	Would we extend past this buffer?
		//
		if ( (CurrPixelOffset + char_len) > BufferList[BufferList.Count () - 1].Length ) {
			needs_new_buffer = true;
		}
	}

	//
	//	Do we need to create a new surface?
	//
	if (needs_new_buffer)
	{
		// TheSuperHackers @fix arcticdolphin 07/09/2026 Length may exceed CHAR_BUFFER_LEN to fit this glyph.
		// GeneralsX: Zero-initialised, where upstream's array is not. The pooled
		// FontCharsBuffer this replaces came from MemoryPool::allocateBlock, which memsets the block, and
		// Store_Freetype_Char writes only the glyph bitmap's pixels into its cell: every other pixel of the
		// cell relies on starting as 0 (transparent). Upstream's GDI-only path writes the whole cell.
		const int length = max( (int)CHAR_BUFFER_LEN, char_len );
		BufferList.Add( FontCharsBuffer( length, W3DNEWARRAY uint32[length]() ) );
		CurrPixelOffset = 0;
	}

	return true;
}

#if defined(SAGE_USE_FREETYPE) && !defined(_WIN32)

#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE

#include <cctype>
#include <cstdio>
#include <unistd.h>

////////////////////////////////////////////////////////////////////////////////////
//
//	Locate_Font_FontConfig (iOS)
//
// iOS has no fontconfig and no user-accessible system font files. Fonts are
// resolved from a "fonts" directory below the current working directory (the
// app's Documents folder, where game data also lives). The requested face name
// is normalized (lowercase, spaces stripped) and tried as <name>.ttf/.otf/.ttc;
// arial.ttf serves as the universal fallback since the game UI is Arial-based.
////////////////////////////////////////////////////////////////////////////////////
const char *
FontCharsClass::Locate_Font_FontConfig (const char *font_name)
{
	char normalized[128];
	int n = 0;
	for ( const char *p = font_name; *p != '\0' && n < (int)sizeof(normalized) - 1; ++p ) {
		if ( *p == ' ' ) {
			continue;
		}
		normalized[n++] = (char)tolower( (unsigned char)*p );
	}
	normalized[n] = '\0';

	static const char *extensions[] = { ".ttf", ".otf", ".ttc" };
	char candidate[256];
	for ( size_t i = 0; i < sizeof(extensions) / sizeof(extensions[0]); ++i ) {
		snprintf( candidate, sizeof(candidate), "fonts/%s%s", normalized, extensions[i] );
		if ( access( candidate, R_OK ) == 0 ) {
			FreetypeFontPath = candidate;
			return FreetypeFontPath;
		}
	}

	// Fall back to the Arial-equivalent face shipped with the app
	if ( access( "fonts/arial.ttf", R_OK ) == 0 ) {
		FreetypeFontPath = "fonts/arial.ttf";
		return FreetypeFontPath;
	}

	return nullptr;
}

#else // !TARGET_OS_IPHONE

////////////////////////////////////////////////////////////////////////////////////
//
//	Locate_Font_FontConfig
//
// TheSuperHackers @feature FreeType port 10/02/2026 Locate system font using Fontconfig
////////////////////////////////////////////////////////////////////////////////////
const char *
FontCharsClass::Locate_Font_FontConfig (const char *font_name)
{
	//
	//	Initialize Fontconfig library
	//
	// GeneralsX: Load the Fontconfig configuration once per process.
	// FcInitLoadConfigAndFonts parses fonts.conf/conf.d and loads every font cache (6-36 ms warm,
	// over a second on a cache revalidation) and ran for every new font (~50 per launch, 4 inside
	// match load). The inputs (config files, FONTCONFIG_* env set before launch, font dirs) do not
	// change during a run, so every match sees the same font set. Intentionally never destroyed.
	static FcConfig *const config = FcInitLoadConfigAndFonts();
	if ( config == nullptr ) {
		return nullptr;
	}

	//
	//	Create a pattern for the requested font
	//
	FcPattern *pattern = FcNameParse( (const FcChar8*)font_name );
	if ( pattern == nullptr ) {
		return nullptr;
	}

	//
	//	Configure the pattern
	//
	FcConfigSubstitute( config, pattern, FcMatchPattern );
	FcDefaultSubstitute( pattern );

	//
	//	Find the best match
	//
	FcResult result = FcResultNoMatch;
	FcPattern *font = FcFontMatch( config, pattern, &result );

	const char *font_path = nullptr;
	if ( font != nullptr && result == FcResultMatch ) {
		//
		//	Extract the font file path
		//
		FcChar8 *file_path = nullptr;
		if ( FcPatternGetString( font, FC_FILE, 0, &file_path ) == FcResultMatch ) {
			FreetypeFontPath = (const char*)file_path;
			font_path = FreetypeFontPath;
		}

		FcPatternDestroy( font );
	}

	FcPatternDestroy( pattern );

	return font_path;
}

#endif // !TARGET_OS_IPHONE


////////////////////////////////////////////////////////////////////////////////////
//
//	Create_Freetype_Font
//
// GeneralsX @build fbraz 11/02/2026 -  Initialize FreeType font (fighter19 pattern)
////////////////////////////////////////////////////////////////////////////////////
bool
FontCharsClass::Create_Freetype_Font (const char *font_name)
{
	//
	//	Initialize FreeType library
	//
	FT_Error error = FT_Init_FreeType( &FTLibrary );
	if ( error != 0 ) {
		return false;
	}

	// GeneralsX: select the v35 TrueType interpreter: FreeType's default v40 does not grid-fit horizontal stems, so at
	// small sizes a stem is ~2px of partial coverage with no solid pixel (a 12pt 'l' has none); v35 grid-fits both axes.
	// Set on FTLibrary, not globally: Create_Freetype_Font builds a fresh FT_Library per font.
	// "GXHUD:<name>" = a GeneralsX panel font (gxtextsmooth.h): FreeType LIGHT hinting (vertical only) instead of v35.
	if (gxTextSmoothName( font_name, &font_name )) {
		Supersample = GX_TEXT_SMOOTH_SS;		// here only the "smooth" flag; the glyphs are rendered once, at 1x
	}
	if (Supersample == 1)
	{
		FT_UInt interpreterVersion = TT_INTERPRETER_VERSION_35;
		// Failure is not fatal - a FreeType built without TT_CONFIG_OPTION_BYTECODE_INTERPRETER
		// simply keeps its default, which is the behaviour we had before this call existed.
		FT_Property_Set( FTLibrary, "truetype", "interpreter-version", &interpreterVersion );
	}

	//
	//	Handle "Generals" font mapping to Arial
	//
	bool doingGenerals = false;
	if ( strcmp( font_name, "Generals" ) == 0 ) {
		font_name = "Arial";
		doingGenerals = true;
	}

	//
	//	Calculate font height in pixels (96 DPI standard)
	//
	const int dotsPerInch = 96;
	int font_height = FT_MulDiv( PointSize, dotsPerInch, 72 );

	//
	//	Locate the font file using Fontconfig
	//
	const char *font_path = Locate_Font_FontConfig( font_name );
	if ( font_path == nullptr ) {
		FT_Done_FreeType( FTLibrary );
		FTLibrary = nullptr;
		return false;
	}

	//
	//	Load the font face
	//
	error = FT_New_Face( FTLibrary, font_path, 0, &FTFace );
	if ( error != 0 ) {
		FT_Done_FreeType( FTLibrary );
		FTLibrary = nullptr;
		return false;
	}

	//
	//	Set the font size (using pixel sizes for simplicity)
	//
	error = FT_Set_Pixel_Sizes( FTFace, 0, font_height );
	if ( error != 0 ) {
		FT_Done_Face( FTFace );
		FT_Done_FreeType( FTLibrary );
		FTFace = nullptr;
		FTLibrary = nullptr;
		return false;
	}

	//
	//	Calculate font metrics (Wine-compatible, same as fighter19)
	//
	if ( FT_IS_SCALABLE( FTFace ) ) {
		CharAscent = FT_MulFix( FTFace->ascender, FTFace->size->metrics.y_scale ) >> 6;
		int descent = -FT_MulFix( FTFace->descender, FTFace->size->metrics.y_scale ) >> 6;
		CharHeight = CharAscent + descent;
		CharOverhang = 0;
	} else {
		//
		//	Non-scalable fonts not supported
		//
		FT_Done_Face( FTFace );
		FT_Done_FreeType( FTLibrary );
		FTFace = nullptr;
		FTLibrary = nullptr;
		return false;
	}

	// GeneralsX @bugfix fbraz 03/06/2026 Log FreeType font details for Cyrillic font issue
	{
		char log_buffer[512];
		snprintf(log_buffer, sizeof(log_buffer),
			"[GX-ISSUE144] Freetype path=%s name=%s family=%s num_glyphs=%ld has_Cyrillic_Caps=%s",
			font_path,
			font_name,
			FTFace->family_name ? FTFace->family_name : "<null>",
			FTFace->num_glyphs,
			FT_Get_Char_Index(FTFace, 0x0410) != 0 ? "YES" : "NO");
		fprintf(stderr, "%s\n", log_buffer);
	}

	if ( doingGenerals ) {
		CharOverhang = 0;
	}

	//
	//	Calculate pixel overlap (same logic as GDI version)
	//
	PixelOverlap = (-font_height) / 8;
	if ( PixelOverlap < 0 ) PixelOverlap = 0;
	if ( PixelOverlap > 4 ) PixelOverlap = 4;

	return true;
}


////////////////////////////////////////////////////////////////////////////////////
//
//	Store_Freetype_Char
//
// GeneralsX @build fbraz 11/02/2026 - FreeType character rendering (fighter19 pattern)
////////////////////////////////////////////////////////////////////////////////////
const FontCharsClassCharDataStruct *
FontCharsClass::Store_Freetype_Char (WCHAR ch)
{
	//
	//	Get the glyph index for the character
	//
	FT_UInt glyph_index = FT_Get_Char_Index( FTFace, ch );

	//
	//	Load the glyph (without rendering yet)
	//
	FT_Error error = FT_Load_Glyph( FTFace, glyph_index, Supersample > 1 ? FT_LOAD_TARGET_LIGHT : FT_LOAD_DEFAULT );
	if ( error != 0 ) {
		return nullptr;
	}

	// GeneralsX: Locate_Font_FontConfig() gets only the font NAME, so a bold request resolved to the regular face.
	// The outline is emboldened instead, which works for every font and platform, including ones with no bold file.
	// The advance is widened by the same amount; otherwise bold glyphs grow into each other and read tighter than regular.
	if ( IsBold && FTFace->glyph->format == FT_GLYPH_FORMAT_OUTLINE ) {
		// 1/24 em is the conventional synthetic-bold strength: heavy enough to read as bold,
		// light enough not to fill in the counters of small glyphs like 'e' and 'a' at 8pt.
		const FT_Pos strength = (FT_Pos)FT_MulFix( FTFace->units_per_EM,
			FTFace->size->metrics.y_scale ) / 24;
		FT_Outline_Embolden( &FTFace->glyph->outline, strength );
		FTFace->glyph->advance.x += strength;
	}

	//
	//	Convert to an anti-aliased bitmap
	//
	error = FT_Render_Glyph( FTFace->glyph, FT_RENDER_MODE_NORMAL );
	if ( error != 0 ) {
		return nullptr;
	}

	FT_GlyphSlot glyph = FTFace->glyph;

	//
	//	Calculate X position (special case for 'W')
	//
	int x_pos = 0;
	if ( ch == 'W' ) {
		x_pos = 1;
	}

	//
	//	Calculate character width (advance + overlap)
	//
	unsigned int char_width = glyph->advance.x >> 6;

	//
	//	Sometimes bitmap is wider than advancement (fix it)
	//
	if ( char_width < glyph->bitmap.width + glyph->bitmap_left ) {
		char_width = glyph->bitmap.width + glyph->bitmap_left;
	}
	char_width += PixelOverlap + x_pos;

	//
	//	Get a pointer to the buffer for this character (allocates if needed)
	//
	if ( !Update_Current_Buffer( (int)char_width ) ) {
		return nullptr;
	}
	uint32 *curr_buffer_p = BufferList[BufferList.Count() - 1].Buffer;
	curr_buffer_p += CurrPixelOffset;

	//
	//	Calculate bitmap offsets (match GDI baseline)
	//
	int x_offset = glyph->bitmap_left;
	int descent = CharHeight - CharAscent;
	int y_offset = (CharHeight - glyph->bitmap_top) - descent;

	//
	//	Prevent invalid buffer access
	//
	if ( x_offset < 0 ) x_offset = 0;
	if ( y_offset < 0 ) y_offset = 0;

	//
	//	Copy FreeType bitmap to our buffer (convert 8-bit gray → 16-bit format)
	//
	// Clip the blit to this character's cell: a substitute font with other vertical metrics (Liberation for Arial on iOS)
	// otherwise spills into the top rows of the next packed cell, leaving slivers above menu text.
	for ( unsigned int row = 0; row < glyph->bitmap.rows; row++ ) {
		const int dst_row = y_offset + (int)row;
		if ( dst_row < 0 || dst_row >= CharHeight ) {
			continue;
		}

		int src_index = row * glyph->bitmap.pitch;
		int dst_index = dst_row * char_width;

		for ( unsigned int col = 0; col < glyph->bitmap.width; col++ ) {
			const int dst_col = x_offset + (int)col;
			if ( dst_col < 0 || dst_col >= char_width ) {
				continue;
			}
			//
			//	Get 8-bit grayscale pixel
			//
			uint8 pixel_value = glyph->bitmap.buffer[src_index + col];

			//
			//	White RGB with the rasteriser's FULL 8-bit coverage as alpha. Same format as the
			//	GDI path above, which is the contract Blit_Char depends on.
			//
			curr_buffer_p[dst_index + dst_col] = ((uint32)Gx_Text_Coverage( Supersample > 1 ? gxSmoothWeight(pixel_value) : pixel_value ) << 24) | 0x00FFFFFF;
		}
	}

	//
	//	Save information about this character
	//
	FontCharsClassCharDataStruct *char_data = W3DNEW FontCharsClassCharDataStruct;
	char_data->Value = ch;
	char_data->Width = (short)char_width;
	char_data->Buffer = BufferList[BufferList.Count() - 1].Buffer + CurrPixelOffset;

	//
	//	Insert into character array (ASCII or Unicode)
	//
	if ( ch < 256 ) {
		ASCIICharArray[ch] = char_data;
	} else {
		UnicodeCharArray[ch - FirstUnicodeChar] = char_data;
	}

	//
	//	Advance the pixel offset for next character
	//
	CurrPixelOffset += (char_width * CharHeight);

	//
	//	Return the character data
	//
	return char_data;
}


////////////////////////////////////////////////////////////////////////////////////
//
//	Free_Freetype_Font
//
// GeneralsX @build fbraz 11/02/2026 - Cleanup FreeType resources
////////////////////////////////////////////////////////////////////////////////////
void
FontCharsClass::Free_Freetype_Font (void)
{
	//
	//	Free the FreeType face
	//
	if ( FTFace != nullptr ) {
		FT_Done_Face( FTFace );
		FTFace = nullptr;
	}

	//
	//	Free the FreeType library
	//
	if ( FTLibrary != nullptr ) {
		FT_Done_FreeType( FTLibrary );
		FTLibrary = nullptr;
	}
}

#endif // SAGE_USE_FREETYPE && !_WIN32


////////////////////////////////////////////////////////////////////////////////////
//
//	Initialize_GDI_Font
//
////////////////////////////////////////////////////////////////////////////////////
bool
FontCharsClass::Initialize_GDI_Font (const char *font_name, int point_size, bool is_bold)
{
	//
	//	Build a unique name from the font name and its size
	//
	Name.Format ("%s%d", font_name, point_size);

	//
	//	Remember these settings
	//
	GDIFontName	= font_name;
	PointSize	= point_size;
	IsBold		= is_bold;

	//
	//	Create the actual font object (platform-specific)
	//  TheSuperHackers @feature FreeType port 10/02/2026 Dispatch to FreeType on Linux
	//
#if defined(SAGE_USE_FREETYPE) && !defined(_WIN32)
	return Create_Freetype_Font (font_name);
#else
	return Create_GDI_Font (font_name);
#endif
}


////////////////////////////////////////////////////////////////////////////////////
//
//	Is_Font
//
////////////////////////////////////////////////////////////////////////////////////
bool
FontCharsClass::Is_Font (const char *font_name, int point_size, bool is_bold)
{
	bool retval = false;

	//
	//	Check to see if both the name and height matches...
	//
	if (	(GDIFontName.Compare_No_Case (font_name) == 0) &&
			(point_size == PointSize) &&
			(is_bold == IsBold))
	{
		retval = true;
	}

	return retval;
}


////////////////////////////////////////////////////////////////////////////////////
//
//	Grow_Unicode_Array
//
////////////////////////////////////////////////////////////////////////////////////
void
FontCharsClass::Grow_Unicode_Array (WCHAR ch)
{
	//
	//	Don't do anything if character is in the ASCII range
	//
	if ( ch < 256 ) {
		return ;
	}

	//
	//	Don't do anything if character is in the currently allocated range
	//
	if ( ch >= FirstUnicodeChar && ch <= LastUnicodeChar ) {
		return ;
	}

	uint16 first_index	= min( FirstUnicodeChar, static_cast<uint16>(ch) );
	uint16 last_index		= max( LastUnicodeChar, static_cast<uint16>(ch) );
	uint16 count			= (last_index - first_index) + 1;

	//
	//	Allocate enough memory to hold the new cells
	//
	FontCharsClassCharDataStruct **new_array = W3DNEWARRAY FontCharsClassCharDataStruct *[count];
	::memset (new_array, 0, sizeof (FontCharsClassCharDataStruct *) * count);

	//
	//	Copy the contents of the old array into the new array
	//
	if ( UnicodeCharArray != nullptr ) {
		int start_offset	= (FirstUnicodeChar - first_index);
		int old_count		= (LastUnicodeChar - FirstUnicodeChar) + 1;
		::memcpy (&new_array[start_offset], UnicodeCharArray, sizeof (FontCharsClassCharDataStruct *) * old_count);

		//
		//	Delete the old array
		//
		delete [] UnicodeCharArray;
		UnicodeCharArray = nullptr;
	}

	FirstUnicodeChar	= first_index;
	LastUnicodeChar	= last_index;
	UnicodeCharArray	= new_array;
}


////////////////////////////////////////////////////////////////////////////////////
//
//	Free_Character_Arrays
//
////////////////////////////////////////////////////////////////////////////////////
void
FontCharsClass::Free_Character_Arrays ()
{
	if ( UnicodeCharArray != nullptr ) {

		int count = (LastUnicodeChar - FirstUnicodeChar) + 1;

		//
		//	Delete each member of the unicode array
		//
		for (int index = 0; index < count; index ++) {
			delete UnicodeCharArray[index];
			UnicodeCharArray[index] = nullptr;
		}

		//
		//	Delete the array itself
		//
		delete [] UnicodeCharArray;
		UnicodeCharArray = nullptr;
	}

	//
	//	Delete each member of the ascii character array
	//
	for (int index = 0; index < 256; index ++) {
		delete ASCIICharArray[index];
		ASCIICharArray[index] = nullptr;
	}
}
