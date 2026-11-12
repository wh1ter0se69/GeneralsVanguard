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

// GLM_ENABLE_EXPERIMENTAL is passed via target_compile_definitions in CMakeLists.txt.
// Do NOT redefine it here -- that causes -Wmacro-redefined with Clang.

#include "d3dx8core.h"
#include "d3dx8_boxfilter.h"

HRESULT WINAPID3DXGetErrorStringA(HRESULT hr, LPSTR pBuffer, UINT BufferLen)
{
	return D3DERR_INVALIDCALL;
}

HRESULT WINAPI
D3DXCreateTexture(LPDIRECT3DDEVICE8 pDevice,
	UINT Width,
	UINT Height,
	UINT MipLevels,
	DWORD Usage,
	D3DFORMAT Format,
	D3DPOOL Pool,
	LPDIRECT3DTEXTURE8 *ppTexture)
{
	// GeneralsX @bugfix fbraz 04/05/2026 Headless replay can request texture creation before DX8 device initialization.
	if (ppTexture == nullptr || pDevice == nullptr)
	{
		if (ppTexture != nullptr)
		{
			*ppTexture = nullptr;
		}

		return D3DERR_INVALIDCALL;
	}

	return pDevice->CreateTexture(Width, Height, MipLevels, Usage, Format, Pool, ppTexture);
}

#include <iostream>

HRESULT WINAPI
D3DXCreateTextureFromFileExA(
	LPDIRECT3DDEVICE8 pDevice,
	LPCSTR pSrcFile,
	UINT Width,
	UINT Height,
	UINT MipLevels,
	DWORD Usage,
	D3DFORMAT Format,
	D3DPOOL Pool,

	DWORD Filter,
	DWORD MipFilter,
	D3DCOLOR ColorKey,
	D3DXIMAGE_INFO *pSrcInfo,
	PALETTEENTRY *pPalette,

	LPDIRECT3DTEXTURE8 *ppTexture)
{
	return D3DERR_INVALIDCALL;
}

HRESULT WINAPI
D3DXLoadSurfaceFromSurface(
	LPDIRECT3DSURFACE8 pDestSurface,
	CONST PALETTEENTRY *pDestPalette,
	CONST RECT *pDestRect,
	LPDIRECT3DSURFACE8 pSrcSurface,
	CONST PALETTEENTRY *pSrcPalette,
	CONST RECT *pSrcRect,
	DWORD Filter,
	D3DCOLOR ColorKey)
{
	D3DSURFACE_DESC descSrc;
	D3DSURFACE_DESC descDest;

	//DEBUG_ASSERTCRASH(pDestPalette == NULL);

	pSrcSurface->GetDesc(&descSrc);
	pDestSurface->GetDesc(&descDest);

	if (descSrc.Format != descDest.Format)
	{
		// Currently we only support scaling between formats of the same type
		return D3DERR_INVALIDCALL;
	}

	// A failed lock leaves pBits unset, and both paths below write through it:
	// refuse instead, releasing only the lock that was taken.
	D3DLOCKED_RECT srcRect;
	if (FAILED(pSrcSurface->LockRect(&srcRect, NULL, 0)))
	{
		return D3DERR_INVALIDCALL;
	}

	D3DLOCKED_RECT destRect;
	if (FAILED(pDestSurface->LockRect(&destRect, NULL, 0)))
	{
		pSrcSurface->UnlockRect();
		return D3DERR_INVALIDCALL;
	}

	// Fast path: No scaling needs to be done if the dimensions are the same
	if (descDest.Width == descSrc.Width && descDest.Height == descSrc.Height)
	{
		// Copy the data directly
		memcpy(destRect.pBits, srcRect.pBits, srcRect.Pitch * descSrc.Height);
		pDestSurface->UnlockRect();
		pSrcSurface->UnlockRect();
		return D3D_OK;
	}

	// The same 2:1 box on every platform (d3dx8_boxfilter.h). Windows and
	// Linux used gli::generate_mipmaps(FILTER_LINEAR), which point-sampled half of every level; Apple already ran this box.
	// It honours both pitches (GLI read the source as if tight and only compared the destination's byte count, so it also
	// accepted a same-size but differently shaped destination). Anything else is refused, and every refusal now unlocks both
	// surfaces (the GLI path returned with both still locked on an unexpected format).
	d3dx8box::Layout layout = d3dx8box::LAYOUT_UNSUPPORTED;
	switch (descSrc.Format)
	{
		case D3DFMT_A8R8G8B8:
		case D3DFMT_X8R8G8B8:
			layout = d3dx8box::LAYOUT_8888;
			break;
		case D3DFMT_A1R5G5B5:
		case D3DFMT_X1R5G5B5:
			layout = d3dx8box::LAYOUT_1555;
			break;
		case D3DFMT_R5G6B5:
			layout = d3dx8box::LAYOUT_565;
			break;
		case D3DFMT_A4R4G4B4:
		case D3DFMT_X4R4G4B4:
			layout = d3dx8box::LAYOUT_4444;
			break;
		default:
			break;
	}

	const bool reduced = d3dx8box::reduceLevel(layout, srcRect.pBits, srcRect.Pitch, descSrc.Width, descSrc.Height,
		destRect.pBits, destRect.Pitch, descDest.Width, descDest.Height);

	pDestSurface->UnlockRect();
	pSrcSurface->UnlockRect();
	return reduced ? D3D_OK : D3DERR_INVALIDCALL;
}

HRESULT WINAPI
D3DXGetErrorStringA(
	HRESULT hr,
	LPSTR pBuffer,
	UINT BufferLen)
{
	return D3DERR_INVALIDCALL;
}

// Taken and adopted from Wine 3.21
HRESULT WINAPI
D3DXFilterTexture(
	LPDIRECT3DBASETEXTURE8 pBaseTexture,
	CONST PALETTEENTRY *pPalette,
	UINT SrcLevel,
	DWORD Filter)
{
	HRESULT hr = D3DERR_INVALIDCALL;
	if (SrcLevel == D3DX_DEFAULT)
	{
		SrcLevel = 0;
	}
	else if (SrcLevel >= pBaseTexture->GetLevelCount())
	{
		return D3DERR_INVALIDCALL;
	}

	D3DRESOURCETYPE type;
	switch (type = pBaseTexture->GetType())
	{
		case D3DRTYPE_TEXTURE:
		{
			IDirect3DTexture8 *tex = (IDirect3DTexture8 *)pBaseTexture;
			IDirect3DSurface8 *topsurf, *mipsurf;
			D3DSURFACE_DESC desc;

			tex->GetLevelDesc(SrcLevel, &desc);
			if (Filter == D3DX_DEFAULT)
			{
				Filter = D3DX_FILTER_BOX;
			}

			int Level = SrcLevel + 1;
			hr = tex->GetSurfaceLevel(SrcLevel, &topsurf);
			if (FAILED(hr))
			{
				return hr;
			}

			// Stop at the first level that could not be built and return why:
			// the loop used to build every later level from it and report D3D_OK. Each surface reference is now
			// released once - the old loop released the last level twice and never the top one.
			while (tex->GetSurfaceLevel(Level, &mipsurf) == D3D_OK)
			{
				hr = D3DXLoadSurfaceFromSurface(mipsurf, NULL, NULL, topsurf, NULL, NULL, Filter, 0);

				topsurf->Release();
				topsurf = mipsurf;
				if (FAILED(hr))
				{
					break;
				}

				Level++;
			}

			topsurf->Release();
			if (FAILED(hr))
			{
				return hr;
			}
		}
		return D3D_OK;
	}

	return D3D_OK;
}

HRESULT WINAPI
D3DXCreateCubeTexture(
	LPDIRECT3DDEVICE8 pDevice,
	UINT Size,
	UINT MipLevels,
	DWORD Usage,
	D3DFORMAT Format,
	D3DPOOL Pool,
	LPDIRECT3DCUBETEXTURE8 *ppCubeTexture)
{
	// Also unused by the game
	return D3DERR_INVALIDCALL;
}

HRESULT WINAPI
D3DXCreateVolumeTexture(
	LPDIRECT3DDEVICE8 pDevice,
	UINT Width,
	UINT Height,
	UINT Depth,
	UINT MipLevels,
	DWORD Usage,
	D3DFORMAT Format,
	D3DPOOL Pool,
	LPDIRECT3DVOLUMETEXTURE8 *ppVolumeTexture)
{
	// Used to create volume textures by the texture loader
	// Actually not used in the game
	return D3DERR_INVALIDCALL;
}

HRESULT WINAPI
D3DXAssembleShader(
	LPCVOID pSrcData,
	UINT SrcDataLen,
	DWORD Flags,
	LPD3DXBUFFER *ppConstants,
	LPD3DXBUFFER *ppCompiledShader,
	LPD3DXBUFFER *ppCompilationErrors)
{
	// Called to create water shader amongst other things
	// Code seems to be handle assembly failing
	return D3DERR_INVALIDCALL;
}

HRESULT WINAPI
D3DXAssembleShaderFromFileA(
	LPCSTR pSrcFile,
	DWORD Flags,
	LPD3DXBUFFER *ppConstants,
	LPD3DXBUFFER *ppCompiledShader,
	LPD3DXBUFFER *ppCompilationErrors)
{
	// Not required by the game
	return D3DERR_INVALIDCALL;
}

// Taken from Wine at ca46949
static UINT Get_TexCoord_Size_From_FVF(DWORD FVF, int tex_num)
{
    return (((((FVF) >> (16 + (2 * (tex_num)))) + 1) & 0x03) + 1);
}

UINT WINAPI D3DXGetFVFVertexSize(DWORD FVF)
{
    DWORD size = 0;
    UINT i;
    UINT numTextures = (FVF & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT;

    if (FVF & D3DFVF_NORMAL) size += sizeof(D3DVECTOR);
    if (FVF & D3DFVF_DIFFUSE) size += sizeof(DWORD);
    if (FVF & D3DFVF_SPECULAR) size += sizeof(DWORD);
    if (FVF & D3DFVF_PSIZE) size += sizeof(DWORD);

    switch (FVF & D3DFVF_POSITION_MASK)
    {
        case D3DFVF_XYZ:    size += sizeof(D3DVECTOR); break;
        case D3DFVF_XYZRHW: size += 4 * sizeof(FLOAT); break;
        case D3DFVF_XYZB1:  size += 4 * sizeof(FLOAT); break;
        case D3DFVF_XYZB2:  size += 5 * sizeof(FLOAT); break;
        case D3DFVF_XYZB3:  size += 6 * sizeof(FLOAT); break;
        case D3DFVF_XYZB4:  size += 7 * sizeof(FLOAT); break;
        case D3DFVF_XYZB5:  size += 8 * sizeof(FLOAT); break;
		// Not present in D3D8, thus uncommented
        //case D3DFVF_XYZW:   size += 4 * sizeof(FLOAT); break;
    }

    for (i = 0; i < numTextures; i++)
    {
        size += Get_TexCoord_Size_From_FVF(FVF, i) * sizeof(FLOAT);
    }

    return size;
}
