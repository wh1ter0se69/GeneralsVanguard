// GLM_ENABLE_EXPERIMENTAL is passed via target_compile_definitions in CMakeLists.txt.
// Do NOT redefine it here -- that causes -Wmacro-redefined with Clang.

#ifdef _WIN32
#include <windows.h>
#else
#include "windows_compat.h"
#endif
#include "d3dx8core.h"

#include "d3dx8math.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

// GeneralsX: Match the real D3DX8 contract (checked by scripts/test/d3dx-math-contract-test.sh).
// The two converters copy the SAME mathematical matrix across (row r, column c <-> _rc), which is right for the
// convention-free operations (Multiply, Inverse, Transpose). D3DX is ROW-vector ([x y z 1] * M, translation in
// _41/_42/_43) while GLM's builders are COLUMN-vector (M * v, translation in column 3), so a GLM-built transform must
// be transposed on the way out, and vectors multiply as v * M. Without that, Translation landed in _14/_24/_34 and
// the shroud texgen lost its one-cell border offset (fog drawn a cell toward +X/+Y), RotationZ turned the wrong way
// and Vec3/Vec4Transform computed M * v.

static void ConvertGLMToD3DX (const glm::mat4x4 &glm, D3DXMATRIX &d3dx)
{
	d3dx._11 = glm[0][0];
	d3dx._12 = glm[1][0];
	d3dx._13 = glm[2][0];
	d3dx._14 = glm[3][0];

	d3dx._21 = glm[0][1];
	d3dx._22 = glm[1][1];
	d3dx._23 = glm[2][1];
	d3dx._24 = glm[3][1];

	d3dx._31 = glm[0][2];
	d3dx._32 = glm[1][2];
	d3dx._33 = glm[2][2];
	d3dx._34 = glm[3][2];

	d3dx._41 = glm[0][3];
	d3dx._42 = glm[1][3];
	d3dx._43 = glm[2][3];
	d3dx._44 = glm[3][3];
}

static void ConvertD3DXToGLM (const D3DXMATRIX &d3dx, glm::mat4x4 &glm)
{
	glm[0][0] = d3dx._11;
	glm[1][0] = d3dx._12;
	glm[2][0] = d3dx._13;
	glm[3][0] = d3dx._14;

	glm[0][1] = d3dx._21;
	glm[1][1] = d3dx._22;
	glm[2][1] = d3dx._23;
	glm[3][1] = d3dx._24;

	glm[0][2] = d3dx._31;
	glm[1][2] = d3dx._32;
	glm[2][2] = d3dx._33;
	glm[3][2] = d3dx._34;

	glm[0][3] = d3dx._41;
	glm[1][3] = d3dx._42;
	glm[2][3] = d3dx._43;
	glm[3][3] = d3dx._44;
}

D3DXMATRIX *WINAPI D3DXMatrixInverse(D3DXMATRIX *pOut, FLOAT *pDeterminant, CONST D3DXMATRIX *pM)
{
	glm::mat4x4 m;
	ConvertD3DXToGLM(*pM, m);

	const float det = glm::determinant(m);
	if (pDeterminant)
		*pDeterminant = det;
	if (det == 0.0f)
		return NULL;	// D3DX: NULL when the matrix has no inverse

	glm::mat4x4 inv = glm::inverse(m);
	ConvertGLMToD3DX(inv, *pOut);

	return pOut;
}

D3DXMATRIX *WINAPI D3DXMatrixScaling(D3DXMATRIX *pOut, FLOAT sx, FLOAT sy, FLOAT sz)
{
	glm::mat4x4 m = glm::scale(glm::mat4x4(1.0f), glm::vec3(sx, sy, sz));
	ConvertGLMToD3DX(m, *pOut);
	return pOut;
}

D3DXMATRIX *WINAPI D3DXMatrixTranslation(D3DXMATRIX *pOut, FLOAT x, FLOAT y, FLOAT z)
{
	glm::mat4x4 m = glm::translate(glm::mat4x4(1.0f), glm::vec3(x, y, z));
	ConvertGLMToD3DX(glm::transpose(m), *pOut);
	return pOut;
}

D3DXMATRIX *WINAPI D3DXMatrixMultiply(D3DXMATRIX *pOut, CONST D3DXMATRIX *pM1, CONST D3DXMATRIX *pM2)
{
	glm::mat4x4 m1, m2;
	ConvertD3DXToGLM(*pM1, m1);
	ConvertD3DXToGLM(*pM2, m2);

	glm::mat4x4 m = m1 * m2;
	ConvertGLMToD3DX(m, *pOut);
	return pOut;
}

D3DXVECTOR4 *WINAPI D3DXVec3Transform(D3DXVECTOR4 *pOut, CONST D3DXVECTOR3 *pV, CONST D3DXMATRIX *pM)
{
	glm::vec4 v(pV->x, pV->y, pV->z, 1.0f);
	glm::mat4x4 m;
	ConvertD3DXToGLM(*pM, m);

	glm::vec4 result = v * m;
	pOut->x = result.x;
	pOut->y = result.y;
	pOut->z = result.z;
	pOut->w = result.w;
	return pOut;
}

D3DXMATRIX *WINAPI D3DXMatrixTranspose(D3DXMATRIX *pOut, CONST D3DXMATRIX *pM)
{
	glm::mat4x4 m;
	ConvertD3DXToGLM(*pM, m);

	glm::mat4x4 mTransposed;
	mTransposed = glm::transpose(m);

	ConvertGLMToD3DX(mTransposed, *pOut);
	return pOut;
}

D3DXMATRIX *WINAPI D3DXMatrixRotationZ(D3DXMATRIX *pOut, FLOAT Angle)
{
	glm::mat4x4 m = glm::rotate(glm::mat4x4(1.0f), Angle, glm::vec3(0.0f, 0.0f, 1.0f));
	ConvertGLMToD3DX(glm::transpose(m), *pOut);
	return pOut;
}

D3DXVECTOR4 *WINAPI D3DXVec4Transform(D3DXVECTOR4 *pOut, CONST D3DXVECTOR4 *pV, CONST D3DXMATRIX *pM)
{
	glm::vec4 v(pV->x, pV->y, pV->z, pV->w);
	glm::mat4x4 m;
	ConvertD3DXToGLM(*pM, m);

	glm::vec4 result = v * m;
	pOut->x = result.x;
	pOut->y = result.y;
	pOut->z = result.z;
	pOut->w = result.w;
	return pOut;
}