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

#pragma once

// CRITICAL: windows.h MUST come before d3d8.h
// On Linux: windows.h → windows_base.h (DXVK) → windows_compat.h
// On Windows: windows.h → Windows SDK
#include <windows.h>

#include <d3d8.h>

#ifdef __cplusplus
extern "C"
{
#endif

// D3DXMATRIX: Wrapper around D3DMATRIX with operator overloads
// DXVK D3DMATRIX uses union { struct { _11, _12, ... }; float m[4][4]; }
// Access via m[row][col] for portability
typedef struct D3DXMATRIX : D3DMATRIX
{
#ifdef __cplusplus
  D3DXMATRIX operator *(const D3DXMATRIX &other) const;
  D3DXMATRIX operator *= (const D3DXMATRIX& other);
#endif
} D3DXMATRIX;

typedef D3DVECTOR D3DXVECTOR3;

typedef struct D3DXVECTOR4
{
#ifdef __cplusplus
  D3DXVECTOR4() {}
  D3DXVECTOR4(FLOAT x, FLOAT y, FLOAT z, FLOAT w) : x(x), y(y), z(z), w(w) {}

  operator FLOAT* () { return &x; }
#endif
  FLOAT x, y, z, w;
} D3DXVECTOR4;

#define D3DX_PI 3.141592654f

D3DXMATRIX *WINAPI D3DXMatrixInverse(D3DXMATRIX *pOut, FLOAT *pDeterminant, CONST D3DXMATRIX *pM);
D3DXMATRIX *WINAPI D3DXMatrixScaling(D3DXMATRIX *pOut, FLOAT sx, FLOAT sy, FLOAT sz);
D3DXMATRIX *WINAPI D3DXMatrixTranslation(D3DXMATRIX *pOut, FLOAT x, FLOAT y, FLOAT z);
D3DXMATRIX *WINAPI D3DXMatrixMultiply(D3DXMATRIX *pOut, CONST D3DXMATRIX *pM1, CONST D3DXMATRIX *pM2);
D3DXVECTOR4 *WINAPI D3DXVec3Transform(D3DXVECTOR4 *pOut, CONST D3DXVECTOR3 *pV, CONST D3DXMATRIX *pM);
D3DXMATRIX *WINAPI D3DXMatrixTranspose(D3DXMATRIX *pOut, CONST D3DXMATRIX *pM);
D3DXMATRIX *WINAPI D3DXMatrixRotationZ(D3DXMATRIX *pOut, FLOAT angle);
D3DXVECTOR4 *WINAPI D3DXVec4Transform(D3DXVECTOR4 *pOut, CONST D3DXVECTOR4 *pV, CONST D3DXMATRIX *pM);

#ifdef __cplusplus

inline D3DXMATRIX D3DXMATRIX::operator*(const D3DXMATRIX &other) const
{
  D3DXMATRIX result;
  D3DXMatrixMultiply(&result, this, &other);
  return result;
}
inline D3DXMATRIX D3DXMATRIX::operator *= (const D3DXMATRIX& other)
{
  D3DXMatrixMultiply(this, this, &other);
  return *this;
}

}
#endif