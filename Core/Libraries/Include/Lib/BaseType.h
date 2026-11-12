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

// FILE: BaseType.h ///////////////////////////////////////////////////////////
//
// Project:  RTS3
//
// Basic types and constants
// Author: Michael S. Booth, January 1995, September 2000
//
///////////////////////////////////////////////////////////////////////////////

// tell the compiler to only load this file once

#pragma once

#include "Lib/BaseTypeCore.h"
#include "Lib/trig.h"
#include <string.h>	// memcpy: the float bit reads below (GeneralsX, Determinism-1)
#include <type_traits>	// is_integral: REAL_TO_* converts each argument from its own type (GeneralsX)

//-----------------------------------------------------------------------------
typedef wchar_t WideChar;  ///< multi-byte character representations

//-----------------------------------------------------------------------------
template <typename NUM>
inline NUM sqr(NUM x)
{
	return x*x;
}

template <typename NUM>
inline NUM clamp(NUM lo, NUM val, NUM hi)
{
	if (val < lo) return lo;
	else if (val > hi) return hi;
	else return val;
}

template <typename NUM>
inline int sign(NUM x)
{
	if (x > 0) return 1;
	else if (x < 0) return -1;
	else return 0;
}

// GeneralsX: The one IABS (upstream #3504, 407248bfd). Upstream puts it in
// Precompiled/BaseAlgorithm.h, which this fork does not have; sign/min/max live here, so it does too.
inline Int IABS(Int x)
{
	if (x >= 0)
		return x;

	return -x;
}

template <typename NUM>
inline NUM highestBit(NUM x)
{
	static_assert(sizeof(NUM) <= 8, "NUM must be 8 bytes or less");
	UnsignedInt64 y = static_cast<UnsignedInt64>(x);

	y |= (y >> 1);
	y |= (y >> 2);
	y |= (y >> 4);
	y |= (y >> 8);
	y |= (y >> 16);
	y |= (y >> 32);

	return static_cast<NUM>(y & ~(y >> 1));
}

template <typename PTR>
inline PTR maxPtr(PTR x, PTR y) noexcept
{
	static_assert(std::is_pointer<PTR>::value, "maxPtr is for pointer types only!");

	if (x == nullptr)
		return y;

	if (y == nullptr)
		return x;

	if (x > y)
		return x;

	return y;
}

template <typename PTR>
inline PTR minPtr(PTR x, PTR y) noexcept
{
	static_assert(std::is_pointer<PTR>::value, "minPtr is for pointer types only!");

	if (x == nullptr)
		return y;

	if (y == nullptr)
		return x;

	if (x < y)
		return x;

	return y;
}

// TheSuperHackers @refactor JohnsterID 24/01/2026 Add lowercase min/max templates for GameEngine layer.
// GameEngine code typically uses BaseType.h, but may include WWVegas headers (which define min/max in always.h).
// Header guard prevents duplicate definitions. VC6's <algorithm> lacks std::min/std::max.
#ifndef _MIN_MAX_TEMPLATES_DEFINED_
#define _MIN_MAX_TEMPLATES_DEFINED_

#ifdef min
#undef min
#endif

#ifdef max
#undef max
#endif

template <typename T>
inline T min(T a, T b) { return (a < b) ? a : b; }

template <typename T>
inline T max(T a, T b) { return (a > b) ? a : b; }

#endif // _MIN_MAX_TEMPLATES_DEFINED_

//-----------------------------------------------------------------------------
inline Real rad2deg(Real rad) { return rad * (180/PI); }
inline Real deg2rad(Real rad) { return rad * (PI/180); }

//-----------------------------------------------------------------------------
// For twiddling bits
//-----------------------------------------------------------------------------
// TheSuperHackers @build xezon 17/03/2025 Renames BitTest to BitIsSet to prevent conflict with BitTest macro from winnt.h
#define BitIsSet( x, i ) ( ( (x) & (i) ) != 0 )
#define BitsAreSet( x, i ) ( ( (x) & (i) ) == (i) )
#define BitSet( x, i ) ( (x) |= (i) )
#define BitClear( x, i ) ( (x ) &= ~(i) )
#define BitToggle( x, i ) ( (x) ^= (i) )

//-------------------------------------------------------------------------------------------------

// note, this function depends on the cpu rounding mode, which we set to CHOP every frame,
// but apparently tends to be left in unpredictable modes by various system bits of
// code, so use this function with caution -- it might not round in the way you want.
__forceinline long fast_float2long_round(float f)
{
	long i;

#if defined(_MSC_VER) && _MSC_VER < 1300
	__asm {
		fld [f]
		fistp [i]
	}
#else
	// GeneralsX: Determinism-1 (ROADMAP_2027 M1 item 3, G-G3-04). lroundf is unspecified
	// outside long's range and for NaN, and long is 32 bits on Windows but 64 elsewhere: x86 answers LONG_MIN, arm64
	// saturates, and the Int every caller stores it in then truncates two different longs differently. Clamp to the Int
	// range first, the same on every target. In range (|f| < 2^31) the result is exactly lroundf's, as before.
	if (!(f == f))
		return 0;
	if (f >= 2147483648.0f)
		return 2147483647L;
	if (f <= -2147483648.0f)
		return -2147483647L - 1;
	i = lroundf(f);
#endif

	return i;
}

// super fast float trunc routine, works always (independent of any FPU modes)
// code courtesy of Martin Hoffesommer (grin)
__forceinline float fast_float_trunc(float f)
{
#if defined(_MSC_VER) && _MSC_VER < 1300
  _asm
  {
    mov ecx,[f]
    shr ecx,23
    mov eax,0xff800000
    xor ebx,ebx
    sub cl,127
    cmovc eax,ebx
    sar eax,cl
    and [f],eax
  }
  return f;
#else
  // GeneralsX: memcpy, not *(unsigned *)&f: the same bits and the same machine code, without the
  // strict-aliasing UB a compiler may optimise differently per target (G-G3-03).
  unsigned x;
  memcpy(&x, &f, sizeof(x));
  unsigned char exp = x >> 23;
  int mask = exp < 127 ? 0 : 0xff800000;
  exp -= 127;
  mask >>= exp & 31;
  x &= mask;
  memcpy(&f, &x, sizeof(f));
  return f;
#endif
}

// same here, fast floor function
__forceinline float fast_float_floor(float f)
{
  // GeneralsX: memcpy for the bit reads (see fast_float_trunc); the arithmetic - including
  // retail's floor(-n) = -n-1 on exact negative integers, which the simulation depends on - is unchanged.
  static const unsigned almost1=(126<<23)|0x7fffff;
  unsigned bits;
  memcpy(&bits, &f, sizeof(bits));
  if (bits &0x80000000)
  {
    float a;
    memcpy(&a, &almost1, sizeof(a));
    f-=a;
  }
  return fast_float_trunc(f);
}

// same here, fast ceil function
__forceinline float fast_float_ceil(float f)
{
  // GeneralsX: memcpy for the bit reads; retail's ceil(n) = n+1 on exact positive integers is
  // unchanged (see fast_float_floor).
  static const unsigned almost1=(126<<23)|0x7fffff;
  unsigned bits;
  memcpy(&bits, &f, sizeof(bits));
  if ( (bits &0x80000000)==0)
  {
    float a;
    memcpy(&a, &almost1, sizeof(a));
    f+=a;
  }
  return fast_float_trunc(f);
}

// GeneralsX: Determinism-1 (ROADMAP_2027 M1 item 3, G-G3-04). A C float->integer cast is
// undefined outside the target type's range and for NaN, and the four targets disagree there: arm64 (fcvtzs/fcvtzu)
// saturates and maps NaN to 0, x86-64 (cvttss2si) answers INT_MIN, and a negative float cast to unsigned wraps on x86
// but is 0 on arm64 - a Mac/iPad vs Windows/Linux split with no visible cause. These pin one answer everywhere:
// saturate to the type's range, NaN to 0. For every in-range input the result is exactly the plain cast's.
__forceinline Int gx_real_to_int(Real x)
{
  if (!(x == x)) return 0;
  if (x >= 2147483648.0f) return 2147483647;
  if (x <= -2147483648.0f) return -2147483647 - 1;
  return (Int)x;
}
__forceinline UnsignedInt gx_real_to_unsigned_int(Real x)
{
  if (!(x == x) || x <= -1.0f) return 0u;	// (-1, 0) truncates to 0, which the plain cast already defines
  if (x >= 4294967296.0f) return 4294967295u;
  return (UnsignedInt)x;
}
// GeneralsX: Determinism-1 item 3 (G-G3-05): pow(v, 2) through no library at all. A float's
// square is exact in double (48 significant bits), so this equals any correct pow() bit for bit.
__forceinline double gx_square(double v) { return v * v; }
__forceinline Int gx_double_to_int(double x)
{
  if (!(x == x)) return 0;
  if (x >= 2147483648.0) return 2147483647;
  if (x <= -2147483649.0) return -2147483647 - 1;	// (-2^31-1, -2^31] truncates to INT_MIN, defined
  return (Int)x;
}
__forceinline UnsignedInt gx_double_to_unsigned_int(double x)
{
  if (!(x == x) || x <= -1.0) return 0u;
  if (x >= 4294967296.0) return 4294967295u;
  return (UnsignedInt)x;
}

// GeneralsX: The REAL_TO_* macros took every argument through Real, so an
// integer of 2^24 or more lost its low bits (EMPUpdate's UnsignedInt deadline 16777217 became 16777216) and a double
// rounded to float before it truncated (0.99999999 became 1). Each argument now converts from its OWN type: a Real takes
// the functions above unchanged, so every float input gives the same result; a double saturates in double;
// an integer or enum takes the plain integral conversion (modular, the same on every
// two's-complement target). Anything else (a class with a conversion operator, long double) converts through Real.
__forceinline Int gx_real_to_int(double x) { return gx_double_to_int(x); }
__forceinline UnsignedInt gx_real_to_unsigned_int(double x) { return gx_double_to_unsigned_int(x); }
template <typename T> __forceinline Int gx_real_to_int(T x)
{
  if constexpr (std::is_integral<T>::value || std::is_enum<T>::value)
    return (Int)x;
  else
    return gx_real_to_int((Real)x);
}
template <typename T> __forceinline UnsignedInt gx_real_to_unsigned_int(T x)
{
  if constexpr (std::is_integral<T>::value || std::is_enum<T>::value)
    return (UnsignedInt)x;
  else
    return gx_real_to_unsigned_int((Real)x);
}

//-------------------------------------------------------------------------------------------------
// GeneralsX: Through the saturating helpers above (Determinism-1 item 3). The narrow types
// convert via Int, so an out-of-range value narrows the same way (two's complement) on every target; in range, each is
// the plain cast's result.
#define REAL_TO_INT(x)						(gx_real_to_int(x))
#define REAL_TO_UNSIGNEDINT(x)		(gx_real_to_unsigned_int(x))
#define REAL_TO_SHORT(x)					((Short)gx_real_to_int(x))
#define REAL_TO_UNSIGNEDSHORT(x)	((UnsignedShort)gx_real_to_int(x))
#define REAL_TO_BYTE(x)						((Byte)gx_real_to_int(x))
#define REAL_TO_UNSIGNEDBYTE(x)		((UnsignedByte)gx_real_to_int(x))
#define REAL_TO_CHAR(x)						((Char)gx_real_to_int(x))
#define DOUBLE_TO_REAL(x)					((Real)(x))
#define DOUBLE_TO_INT(x)					(gx_double_to_int((double)(x)))
#define INT_TO_REAL(x)						((Real)(x))

// once we've ceiled/floored, trunc and round are identical, and currently, round is faster... (srj)
#if RTS_GENERALS /*&& RETAIL_COMPATIBLE_CRC*/
#define REAL_TO_INT_CEIL(x)				(fast_float2long_round(ceilf(x)))
#define REAL_TO_INT_FLOOR(x)			(fast_float2long_round(floorf(x)))
#else
#define REAL_TO_INT_CEIL(x)				(fast_float2long_round(fast_float_ceil(x)))
#define REAL_TO_INT_FLOOR(x)			(fast_float2long_round(fast_float_floor(x)))
#endif

#define FAST_REAL_TRUNC(x)        fast_float_trunc(x)
#define FAST_REAL_CEIL(x)         fast_float_ceil(x)
#define FAST_REAL_FLOOR(x)        fast_float_floor(x)

//--------------------------------------------------------------------
// Derived type definitions
//--------------------------------------------------------------------

// NOTE: Keep these derived types simple, and avoid constructors and destructors
// so they can be used within unions.

// real-valued range defined by low and high values
struct RealRange
{
	Real lo, hi;							// low and high values of the range

	void zero()
	{
		lo = 0.0f;
		hi = 0.0f;
	}

	bool is(Real value) const
	{
		return lo == value && hi == value;
	}

	// combine the given range with us such that we now encompass
	// both ranges
	void combine( RealRange &other )
	{
		lo = min( lo, other.lo );
		hi = max( hi, other.hi );
	}
};

struct Coord2D
{
	Real x, y;

	void zero()
	{
		x = 0.0f;
		y = 0.0f;
	}

	bool is(Real value) const
	{
		return x == value && y == value;
	}

	// GeneralsX @refactor fbraz 03/05/2026 Route BaseType length math through shared Sqrt gateway.
	// Upstream reference: Okladnoj, PR #2670
	// https://github.com/TheSuperHackers/GeneralsGameCode/pull/2670
	Real length() const { return (Real)Sqrt( x*x + y*y ); }
	Real lengthSqr() const { return x*x + y*y; }

	void normalize()
	{
		Real len = length();
		if( len != 0 )
		{
			x /= len;
			y /= len;
		}
	}

	Real toAngle() const;  ///< turn 2D vector into angle (where angle 0 is down the +x axis)

	void add( const Coord2D &a )
	{
		x += a.x;
		y += a.y;
	}

	void sub( const Coord2D &a )
	{
		x -= a.x;
		y -= a.y;
	}

	void operator+=( const Coord2D &a )
	{
		add(a);
	}

	void operator-=( const Coord2D &a )
	{
		sub(a);
	}

	Coord2D operator+() const
	{
		return *this;
	}

	Coord2D operator-() const
	{
		Coord2D c;
		c.x = -x;
		c.y = -y;
		return c;
	}

	void set( const Coord2D &a )
	{
		x = a.x;
		y = a.y;
	}

	void set( Real ax, Real ay )
	{
		x = ax;
		y = ay;
	}

	void updateMin( const Coord2D &other )
	{
		if (x > other.x)
			x = other.x;

		if (y > other.y)
			y = other.y;
	}

	void updateMax( const Coord2D &other )
	{
		if (x < other.x)
			x = other.x;

		if (y < other.y)
			y = other.y;
	}
};

inline Coord2D operator+( const Coord2D &a, const Coord2D &b )
{
	Coord2D c = a;
	c.add(b);
	return c;
}

inline Coord2D operator-( const Coord2D &a, const Coord2D &b )
{
	Coord2D c = a;
	c.sub(b);
	return c;
}

inline Real Coord2D::toAngle() const
{
#if RTS_GENERALS /*&& RETAIL_COMPATIBLE_CRC*/
	Coord2D vector;

	vector.x = x;
	vector.y = y;

	Real dist = (Real)Sqrt(vector.x * vector.x + vector.y * vector.y);

	// normalize
	if (dist == 0.0f)
		return 0.0f;

	Coord2D dir;
	dir.x = 1.0f;
	dir.y = 0.0f;

	Real distInv = 1.0f / dist;
	vector.x *= distInv;
	vector.y *= distInv;

	// dot of two unit vectors is cos of angle
	Real c = dir.x*vector.x + dir.y*vector.y;

	// bound it in case of numerical error
	if (c < -1.0)
		c = -1.0;
	else if (c > 1.0)
		c = 1.0;

	Real value = (Real)ACos( (Real)c );

	// Determine sign by checking Z component of dir cross vector
	// Note this is assumes 2D, and is identical to dotting the perpendicular of v with dir
	Real perpZ = dir.x * vector.y - dir.y * vector.x;
	if (perpZ < 0.0f)
		value = -value;

	// note: to make this 3D, 'dir' and 'vector' can be normalized and dotted just as they are
	// to test sign, compute N = dir X vector, then P = N x dir, then S = P . vector, where sign of
	// S is sign of angle - MSB

	return value;
#else
	const Real len = length();
	if (len == 0.0f)
		return 0.0f;

	Real c = x/len;
	// bound it in case of numerical error
	if (c < -1.0f)
		c = -1.0f;
	else if (c > 1.0f)
		c = 1.0f;

	return y < 0.0f ? -ACos(c) : ACos(c);
#endif
}

struct ICoord2D
{
	Int x, y;

	void zero()
	{
		x = 0;
		y = 0;
	}

	bool is(Int value) const
	{
		return x == value && y == value;
	}

	// GeneralsX: Deterministic Sqrt, not libm sqrt - this feeds the lockstep CRC.
	Int length() const { return (Int)Sqrt( (double)(x*x + y*y) ); }
	Int lengthSqr() const { return x*x + y*y; }

	void add( const ICoord2D &a )
	{
		x += a.x;
		y += a.y;
	}

	void sub( const ICoord2D &a )
	{
		x -= a.x;
		y -= a.y;
	}

	void operator+=( const ICoord2D &a )
	{
		add(a);
	}

	void operator-=( const ICoord2D &a )
	{
		sub(a);
	}

	ICoord2D operator+() const
	{
		return *this;
	}

	ICoord2D operator-() const
	{
		ICoord2D c;
		c.x = -x;
		c.y = -y;
		return c;
	}

	void set( const ICoord2D &a )
	{
		x = a.x;
		y = a.y;
	}

	void set( Int ax, Int ay )
	{
		x = ax;
		y = ay;
	}

	void updateMin( const ICoord2D &other )
	{
		if (x > other.x)
			x = other.x;

		if (y > other.y)
			y = other.y;
	}

	void updateMax( const ICoord2D &other )
	{
		if (x < other.x)
			x = other.x;

		if (y < other.y)
			y = other.y;
	}
};

inline ICoord2D operator+( const ICoord2D &a, const ICoord2D &b )
{
	ICoord2D c = a;
	c.add(b);
	return c;
}

inline ICoord2D operator-( const ICoord2D &a, const ICoord2D &b )
{
	ICoord2D c = a;
	c.sub(b);
	return c;
}

struct Region2D
{
	Coord2D lo, hi;						// bounds of 2D rectangular region

	// Keep only the overlapping portion of both regions.
	void intersectWith( const Region2D &other )
	{
		lo.updateMax(other.lo);
		hi.updateMin(other.hi);
	}

	// Expand to include the other region.
	void uniteWith( const Region2D &other )
	{
		lo.updateMin(other.lo);
		hi.updateMax(other.hi);
	}

	// Expand to include the point.
	void uniteWith( const Coord2D &point )
	{
		lo.updateMin(point);
		hi.updateMax(point);
	}

	void zero()
	{
		lo.zero();
		hi.zero();
	}

	bool is(Real value) const
	{
		return lo.is(value) && hi.is(value);
	}

	Real width() const { return hi.x - lo.x; }
	Real height() const { return hi.y - lo.y; }
	Bool isInRegion( Real x, Real y ) const { return (lo.x < x) && (x < hi.x) && (lo.y < y) && (y < hi.y); }
};

struct IRegion2D
{
	ICoord2D lo, hi;					// bounds of 2D rectangular region

	// Keep only the overlapping portion of both regions.
	void intersectWith( const IRegion2D &other )
	{
		lo.updateMax(other.lo);
		hi.updateMin(other.hi);
	}

	// Expand to include the other region.
	void uniteWith( const IRegion2D &other )
	{
		lo.updateMin(other.lo);
		hi.updateMax(other.hi);
	}

	// Expand to include the point.
	void uniteWith( const ICoord2D &point )
	{
		lo.updateMin(point);
		hi.updateMax(point);
	}

	void zero()
	{
		lo.zero();
		hi.zero();
	}

	bool is(Int value) const
	{
		return lo.is(value) && hi.is(value);
	}

	Int width() const { return hi.x - lo.x; }
	Int height() const { return hi.y - lo.y; }
	Bool isInRegion( Int x, Int y ) const { return (lo.x < x) && (x < hi.x) && (lo.y < y) && (y < hi.y); }
};


struct Coord3D
{
	Real x, y, z;

	Coord2D asCoord2D() const
	{
		const Coord2D xy = { x, y };
		return xy;
	}

	Real length() const { return (Real)Sqrt( x*x + y*y + z*z ); }
	Real lengthSqr() const { return ( x*x + y*y + z*z ); }

	void normalize()
	{
		Real len = length();

		if( len != 0 )
		{
			x /= len;
			y /= len;
			z /= len;
		}
	}

	static void crossProduct( const Coord3D &a, const Coord3D &b, Coord3D &r )
	{
		r.x = (a.y * b.z - a.z * b.y);
		r.y = (a.z * b.x - a.x * b.z);
		r.z = (a.x * b.y - a.y * b.x);
	}

	void zero()
	{
		x = 0.0f;
		y = 0.0f;
		z = 0.0f;
	}

	bool is(Real value) const
	{
		return x == value && y == value && z == value;
	}

	void add( const Coord3D &a )
	{
		x += a.x;
		y += a.y;
		z += a.z;
	}

	void sub( const Coord3D &a )
	{
		x -= a.x;
		y -= a.y;
		z -= a.z;
	}

	void operator+=( const Coord3D &a )
	{
		add(a);
	}

	void operator-=( const Coord3D &a )
	{
		sub(a);
	}

	Coord3D operator+() const
	{
		return *this;
	}

	Coord3D operator-() const
	{
		Coord3D c;
		c.x = -x;
		c.y = -y;
		c.z = -z;
		return c;
	}

	void set( const Coord3D &a )
	{
		x = a.x;
		y = a.y;
		z = a.z;
	}

	void set( Real ax, Real ay, Real az )
	{
		x = ax;
		y = ay;
		z = az;
	}

	void scale( Real scale )
	{
		x *= scale;
		y *= scale;
		z *= scale;
	}

	Bool equals( const Coord3D &r )
	{
		return (x == r.x &&
						y == r.y &&
						z == r.z);
	}

	Bool operator==( const Coord3D &r ) const
	{
		return (x == r.x &&
						y == r.y &&
						z == r.z);
	}

	void updateMin( const Coord3D &other )
	{
		if (x > other.x)
			x = other.x;

		if (y > other.y)
			y = other.y;

		if (z > other.z)
			z = other.z;
	}

	void updateMax( const Coord3D &other )
	{
		if (x < other.x)
			x = other.x;

		if (y < other.y)
			y = other.y;

		if (z < other.z)
			z = other.z;
	}
};

inline Coord3D operator+( const Coord3D &a, const Coord3D &b )
{
	Coord3D c = a;
	c.add(b);
	return c;
}

inline Coord3D operator-( const Coord3D &a, const Coord3D &b )
{
	Coord3D c = a;
	c.sub(b);
	return c;
}

struct ICoord3D
{
	Int x, y, z;

	ICoord2D asICoord2D() const
	{
		const ICoord2D xy = { x, y };
		return xy;
	}

	// GeneralsX: Deterministic Sqrt, not libm sqrt - this feeds the lockstep CRC.
	Int length() const { return (Int)Sqrt( (double)(x*x + y*y + z*z) ); }
	Int lengthSqr() const { return x*x + y*y + z*z; }

	void zero()
	{
		x = 0;
		y = 0;
		z = 0;
	}

	bool is(Int value) const
	{
		return x == value && y == value && z == value;
	}

	void add( const ICoord3D &a )
	{
		x += a.x;
		y += a.y;
		z += a.z;
	}

	void sub( const ICoord3D &a )
	{
		x -= a.x;
		y -= a.y;
		z -= a.z;
	}

	void operator+=( const ICoord3D &a )
	{
		add(a);
	}

	void operator-=( const ICoord3D &a )
	{
		sub(a);
	}

	ICoord3D operator+() const
	{
		return *this;
	}

	ICoord3D operator-() const
	{
		ICoord3D c;
		c.x = -x;
		c.y = -y;
		c.z = -z;
		return c;
	}

	void set( const ICoord3D &a )
	{
		x = a.x;
		y = a.y;
		z = a.z;
	}

	void set( Int ax, Int ay, Int az )
	{
		x = ax;
		y = ay;
		z = az;
	}

	void updateMin( const ICoord3D &other )
	{
		if (x > other.x)
			x = other.x;

		if (y > other.y)
			y = other.y;

		if (z > other.z)
			z = other.z;
	}

	void updateMax( const ICoord3D &other )
	{
		if (x < other.x)
			x = other.x;

		if (y < other.y)
			y = other.y;

		if (z < other.z)
			z = other.z;
	}
};

inline ICoord3D operator+( const ICoord3D &a, const ICoord3D &b )
{
	ICoord3D c = a;
	c.add(b);
	return c;
}

inline ICoord3D operator-( const ICoord3D &a, const ICoord3D &b )
{
	ICoord3D c = a;
	c.sub(b);
	return c;
}

// For alternative see AABoxClass
struct Region3D
{
	Coord3D lo, hi;						// axis-aligned bounding box

	// Keep only the overlapping portion of both regions.
	void intersectWith( const Region3D &other )
	{
		lo.updateMax(other.lo);
		hi.updateMin(other.hi);
	}

	// Expand to include the other region.
	void uniteWith( const Region3D &other )
	{
		lo.updateMin(other.lo);
		hi.updateMax(other.hi);
	}

	// Expand to include the point.
	void uniteWith( const Coord3D &point )
	{
		lo.updateMin(point);
		hi.updateMax(point);
	}

	Real width() const { return hi.x - lo.x; }
	Real height() const { return hi.y - lo.y; }
	Real depth() const { return hi.z - lo.z; }

	void zero() { lo.zero(); hi.zero(); }

	bool is(Real value) const
	{
		return lo.is(value) && hi.is(value);
	}

	// Set XY from a 2D region and leave Z unchanged.
	void setXY(const Region2D &region)
	{
		lo.x = region.lo.x;
		lo.y = region.lo.y;
		hi.x = region.hi.x;
		hi.y = region.hi.y;
	}

	// Set XY from any amount of points and leave Z unchanged.
	void setXYFromPoints(const Coord3D* points, Int count)
	{
		lo.x = points[0].x;
		lo.y = points[0].y;
		hi.x = points[0].x;
		hi.y = points[0].y;
		for (Int i = 1; i < count; ++i)
		{
			if (points[i].x < lo.x)
				lo.x = points[i].x;
			if (points[i].y < lo.y)
				lo.y = points[i].y;
			if (points[i].x > hi.x)
				hi.x = points[i].x;
			if (points[i].y > hi.y)
				hi.y = points[i].y;
		}
	}

	void setFromPoints(const Coord3D* points, Int count)
	{
		lo = points[0];
		hi = points[0];
		for (Int i = 1; i < count; ++i)
		{
			uniteWith(points[i]);
		}
	}

	Bool isInRegionNoZ( const Coord3D &query ) const
	{
		return (lo.x < query.x) && (query.x < hi.x) &&
					 (lo.y < query.y) && (query.y < hi.y);
	}

	Bool isInRegion( const Coord3D &query ) const
	{
		return (lo.x < query.x) && (query.x < hi.x) &&
					 (lo.y < query.y) && (query.y < hi.y) &&
					 (lo.z < query.z) && (query.z < hi.z);
	}
};

struct IRegion3D
{
	ICoord3D lo, hi;					// axis-aligned bounding box

	// Keep only the overlapping portion of both regions.
	void intersectWith( const IRegion3D &other )
	{
		lo.updateMax(other.lo);
		hi.updateMin(other.hi);
	}

	// Expand to include the other region.
	void uniteWith( const IRegion3D &other )
	{
		lo.updateMin(other.lo);
		hi.updateMax(other.hi);
	}

	// Expand to include the point.
	void uniteWith( const ICoord3D &point )
	{
		lo.updateMin(point);
		hi.updateMax(point);
	}

	void zero()
	{
		lo.zero();
		hi.zero();
	}

	bool is(Int value) const
	{
		return lo.is(value) && hi.is(value);
	}

	Int width() const { return hi.x - lo.x; }
	Int height() const { return hi.y - lo.y; }
	Int depth() const { return hi.z - lo.z; }
};


struct RGBColor
{
	Real red, green, blue;		// range between 0 and 1

	Int getAsInt() const
	{
		return
			((Int)(red * 255.0) << 16) |
			((Int)(green * 255.0) << 8) |
			((Int)(blue * 255.0) << 0);
	}

	void setFromInt(Int c)
	{
		red = ((c >> 16) & 0xff) / 255.0f;
		green = ((c >>  8) & 0xff) / 255.0f;
		blue = ((c >>  0) & 0xff) / 255.0f;
	}

};

struct RGBAColorReal
{

	Real red, green, blue, alpha;  // range between 0.0 and 1.0

};

struct RGBAColorInt
{

	UnsignedInt red, green, blue, alpha;  // range between 0 and 255

};
