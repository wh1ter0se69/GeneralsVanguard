// GxDxvkMath.h - DXVK 2.6's CPU vector/matrix arithmetic, for the FF state GxPackFF uploads.
//
// Adapted from DXVK 2.6 (fbraz3 fork 954c5c69 = the classic path players run) src/util/util_matrix.cpp / util_matrix.h and
// src/util/util_vector.h - ALTERED (GXRender/third_party/SOURCE.txt: paths, commit, sha256).
// DXVK is Copyright (c) 2017 Philip Rebohle and DXVK contributors, provided under the zlib/libpng licence:
//   This software is provided 'as-is', without any express or implied warranty. In no event will the authors be held liable
//   for any damages arising from the use of this software. Permission is granted to anyone to use this software for any
//   purpose, including commercial applications, and to alter it and redistribute it freely, subject to the following
//   restrictions: 1. The origin of this software must not be misrepresented; you must not claim that you wrote the original
//   software. If you use this software in a product, an acknowledgment in the product documentation would be appreciated but
//   is not required. 2. Altered source versions must be plainly marked as such, and must not be misrepresented as being the
//   original software. 3. This notice may not be removed or altered from any source distribution.
// ALTERED: float only, header-only, renamed into GXRender::dxm, only the operations D3D9DeviceEx::UpdateFixedFunctionVS and
// D3D9Light use, and the floating-point contraction pinned (below). WHY A COPY and not our own maths: the FF VS reads WorldView
// (positions), its inverse (normals) and the view-space lights straight from these results, so every rounding difference is a
// pixel difference against the classic path. What must stay identical is the EXPRESSION SHAPE, because that decides rounding:
// DXVK's Vector4 ops are separate functions (a multiply in one is never fused with an add in another under per-statement
// contraction), while its scalar `coef = a * b - c * d` lines are single statements. Do not "simplify" anything here.
//
// FP CONTRACTION, pinned to what DXVK's shipped builds do (DXVK's meson.build sets no -ffp-contract: each compiler's default):
//   macOS / iOS arm64, Apple clang (cmake/dx8.cmake meson build): per-statement fusion. OBSERVED in libdxvk_d3d9: inverse()
//     = 18 fmadd (its 18 coef lines), Matrix4*Matrix4 and Matrix4*Vector4 = 0 fused ops. -> `#pragma clang fp contract(on)`.
//   Windows x64, mingw-w64 gcc (build-dxvk-win64.ps1) and Linux x86_64 gcc: -msse..-msse3 -mfpmath=sse, no FMA instruction set,
//     so nothing can fuse. -> MSVC `fp_contract (off)`; a GCC build with FMA enabled is refused below.
// GX_DXM_CONTRACT_TEST (TEST-ONLY, never shipped) = the WRONG mode, the arm that shows the mode matters: 1 = clang with fusion
// off; 2 = GCC with these functions fused (FMA, -ffp-contract=fast).
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

#if defined(__GNUC__) && !defined(__clang__)
#if defined(GX_DXM_CONTRACT_TEST) && GX_DXM_CONTRACT_TEST == 2
// TEST-ONLY: the wrong mode for GCC x86 - these functions fused (FMA, contraction across statements), as a build with FMA enabled
// and GCC's default -ffp-contract=fast would compile them; DXVK's x86 builds cannot fuse (no FMA instruction set)
#pragma GCC push_options
#pragma GCC target("fma")
#pragma GCC optimize("fp-contract=fast")
#define GX_DXM_GCC_TEST_POP 1
#elif defined(__FMA__)
#error "GxDxvkMath.h: GCC with FMA enabled would fuse differently from DXVK's x86 builds (no FMA): compile this TU with -ffp-contract=off"
#endif
#endif
#if defined(_MSC_VER) && !defined(__clang__)
#pragma fp_contract(off)
#endif
#if defined(__clang__)
#if defined(GX_DXM_CONTRACT_TEST) && GX_DXM_CONTRACT_TEST == 1
#define GX_DXM_FP _Pragma("clang fp contract(off)")
#else
#define GX_DXM_FP _Pragma("clang fp contract(on)")
#endif
#else
#define GX_DXM_FP
#endif

namespace GXRender { namespace dxm {

struct Vector4 {
	Vector4() : x{}, y{}, z{}, w{} {}
	Vector4(float x, float y, float z, float w) : x(x), y(y), z(z), w(w) {}
	Vector4(const Vector4& other) = default;
	Vector4& operator=(const Vector4& other) = default;
	float& operator[](size_t index) { return data[index]; }
	const float& operator[](size_t index) const { return data[index]; }
	Vector4 operator+(const Vector4& other) const { GX_DXM_FP return { x + other.x, y + other.y, z + other.z, w + other.w }; }
	Vector4 operator-(const Vector4& other) const { GX_DXM_FP return { x - other.x, y - other.y, z - other.z, w - other.w }; }
	Vector4 operator*(float scalar) const { GX_DXM_FP return { scalar * x, scalar * y, scalar * z, scalar * w }; }
	Vector4 operator*(const Vector4& other) const {
		GX_DXM_FP
		Vector4 result;
		for (uint32_t i = 0; i < 4; i++)
			result[i] = data[i] * other.data[i];
		return result;
	}
	union {
		float data[4];
		struct { float x, y, z, w; };
	};
};

inline float dot(const Vector4& a, const Vector4& b) { GX_DXM_FP return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w; }
inline float lengthSqr(const Vector4& a) { return dot(a, a); }
inline float length(const Vector4& a) { GX_DXM_FP return std::sqrt(float(lengthSqr(a))); }
inline Vector4 normalize(const Vector4& a) { GX_DXM_FP return a * float(1.0f / length(a)); }

// Columns, as DXVK: a D3DMATRIX's bytes copied in make column i = the D3D row i, so DXVK's products over those bytes compute
// the D3D (row-vector) products: view * world here = World x View in D3D terms, (m * v) = v x M.
struct Matrix4 {
	Vector4 data[4];
	Vector4& operator[](size_t index) { return data[index]; }
	const Vector4& operator[](size_t index) const { return data[index]; }

	Matrix4 operator*(const Matrix4& m2) const {
		GX_DXM_FP
		const Matrix4& m1 = *this;
		const Vector4 srcA0 = { m1[0] };
		const Vector4 srcA1 = { m1[1] };
		const Vector4 srcA2 = { m1[2] };
		const Vector4 srcA3 = { m1[3] };
		const Vector4 srcB0 = { m2[0] };
		const Vector4 srcB1 = { m2[1] };
		const Vector4 srcB2 = { m2[2] };
		const Vector4 srcB3 = { m2[3] };
		Matrix4 result;
		result[0] = srcA0 * srcB0[0] + srcA1 * srcB0[1] + srcA2 * srcB0[2] + srcA3 * srcB0[3];
		result[1] = srcA0 * srcB1[0] + srcA1 * srcB1[1] + srcA2 * srcB1[2] + srcA3 * srcB1[3];
		result[2] = srcA0 * srcB2[0] + srcA1 * srcB2[1] + srcA2 * srcB2[2] + srcA3 * srcB2[3];
		result[3] = srcA0 * srcB3[0] + srcA1 * srcB3[1] + srcA2 * srcB3[2] + srcA3 * srcB3[3];
		return result;
	}
	Vector4 operator*(const Vector4& v) const {
		GX_DXM_FP
		const Matrix4& m = *this;
		const Vector4 mul0 = { m[0] * v[0] };
		const Vector4 mul1 = { m[1] * v[1] };
		const Vector4 mul2 = { m[2] * v[2] };
		const Vector4 mul3 = { m[3] * v[3] };
		const Vector4 add0 = { mul0 + mul1 };
		const Vector4 add1 = { mul2 + mul3 };
		return add0 + add1;
	}
	Matrix4 operator*(float scalar) const {
		GX_DXM_FP
		Matrix4 mat;
		for (uint32_t i = 0; i < 4; i++)
			mat[i] = data[i] * scalar;
		return mat;
	}
};

inline Matrix4 loadD3D(const float m[16]) {   // the D3DMATRIX bytes, as DXVK keeps them
	Matrix4 r;
	std::memcpy(&r, m, 64);
	return r;
}
inline void store(const Matrix4& m, float out[16]) { std::memcpy(out, &m, 64); }

inline Matrix4 transpose(const Matrix4& m) {
	Matrix4 result;
	for (uint32_t i = 0; i < 4; i++) {
		for (uint32_t j = 0; j < 4; j++)
			result[i][j] = m.data[j][i];
	}
	return result;
}

inline Matrix4 inverse(const Matrix4& m) {
	GX_DXM_FP
	float coef00 = m[2][2] * m[3][3] - m[3][2] * m[2][3];
	float coef02 = m[1][2] * m[3][3] - m[3][2] * m[1][3];
	float coef03 = m[1][2] * m[2][3] - m[2][2] * m[1][3];
	float coef04 = m[2][1] * m[3][3] - m[3][1] * m[2][3];
	float coef06 = m[1][1] * m[3][3] - m[3][1] * m[1][3];
	float coef07 = m[1][1] * m[2][3] - m[2][1] * m[1][3];
	float coef08 = m[2][1] * m[3][2] - m[3][1] * m[2][2];
	float coef10 = m[1][1] * m[3][2] - m[3][1] * m[1][2];
	float coef11 = m[1][1] * m[2][2] - m[2][1] * m[1][2];
	float coef12 = m[2][0] * m[3][3] - m[3][0] * m[2][3];
	float coef14 = m[1][0] * m[3][3] - m[3][0] * m[1][3];
	float coef15 = m[1][0] * m[2][3] - m[2][0] * m[1][3];
	float coef16 = m[2][0] * m[3][2] - m[3][0] * m[2][2];
	float coef18 = m[1][0] * m[3][2] - m[3][0] * m[1][2];
	float coef19 = m[1][0] * m[2][2] - m[2][0] * m[1][2];
	float coef20 = m[2][0] * m[3][1] - m[3][0] * m[2][1];
	float coef22 = m[1][0] * m[3][1] - m[3][0] * m[1][1];
	float coef23 = m[1][0] * m[2][1] - m[2][0] * m[1][1];

	Vector4 fac0 = { coef00, coef00, coef02, coef03 };
	Vector4 fac1 = { coef04, coef04, coef06, coef07 };
	Vector4 fac2 = { coef08, coef08, coef10, coef11 };
	Vector4 fac3 = { coef12, coef12, coef14, coef15 };
	Vector4 fac4 = { coef16, coef16, coef18, coef19 };
	Vector4 fac5 = { coef20, coef20, coef22, coef23 };

	Vector4 vec0 = { m[1][0], m[0][0], m[0][0], m[0][0] };
	Vector4 vec1 = { m[1][1], m[0][1], m[0][1], m[0][1] };
	Vector4 vec2 = { m[1][2], m[0][2], m[0][2], m[0][2] };
	Vector4 vec3 = { m[1][3], m[0][3], m[0][3], m[0][3] };

	Vector4 inv0 = { vec1 * fac0 - vec2 * fac1 + vec3 * fac2 };
	Vector4 inv1 = { vec0 * fac0 - vec2 * fac3 + vec3 * fac4 };
	Vector4 inv2 = { vec0 * fac1 - vec1 * fac3 + vec3 * fac5 };
	Vector4 inv3 = { vec0 * fac2 - vec1 * fac4 + vec2 * fac5 };

	Vector4 signA = { +1, -1, +1, -1 };
	Vector4 signB = { -1, +1, -1, +1 };
	Matrix4 inverse = { { inv0 * signA, inv1 * signB, inv2 * signA, inv3 * signB } };

	Vector4 row0 = { inverse[0][0], inverse[1][0], inverse[2][0], inverse[3][0] };

	Vector4 dot0 = { m[0] * row0 };
	float dot1 = (dot0.x + dot0.y) + (dot0.z + dot0.w);

	if (std::abs(dot1) <= std::numeric_limits<float>::min() * 10)
		return m;

	return inverse * (1.0f / dot1);
}

}} // namespace GXRender::dxm

#undef GX_DXM_FP
#ifdef GX_DXM_GCC_TEST_POP
#pragma GCC pop_options
#undef GX_DXM_GCC_TEST_POP
#endif
