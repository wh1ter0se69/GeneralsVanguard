/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2026 TheSuperHackers
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

#include "PreRTS.h"

#include "Common/Diagnostic/SimulationMathCrc.h"
#include "Common/XferCRC.h"
#include "WWMath/matrix3d.h"
#include "WWMath/wwmath.h"
#include "GameLogic/FPUControl.h"

#include <math.h>
#ifndef _WIN32
#include <fenv.h>
#endif

// GeneralsX @diag Claude 27/07/2026 De-fold the math probe so it measures libm, not the compiler.
//
// Every argument below was a compile-time literal, so the compiler evaluated the calls itself and
// the resulting CRC compared two CONSTANT FOLDERS rather than two math libraries. Measured with
// Apple clang arm64 at both -O2 and -O3 with -ffp-contract=off (cmake/compilers.cmake:68): exactly
// 10 of the 17 libm call sites were folded into literal-pool constants - sinf, cosf, both powf,
// both log10f, atan2f, sqrtf, expf and logf - leaving only tanf, asinf, acosf, atanf, sinhf, coshf
// and tanhf as real calls. MSVC folds a different subset, so the value was close to meaningless as
// a cross-platform comparison.
//
// Routing each argument through a volatile object makes its value unknown at translation time and
// makes the read an observable side effect the standard forbids eliding, so every call must be
// emitted and executed. volatile rather than an asm barrier because MSVC x64 has no inline asm,
// and unlike a barrier volatile is immune to LTO.
//
// Both powf arguments and both atan2f arguments are laundered, not just the first: given a literal
// 2.0f exponent clang rewrites powf(x, 2.0f) into a single fmul and powf is never called at all.
// sqrtf still lowers to fsqrt/sqrtss on both targets and is IEEE-754 correctly rounded either way,
// so it is a hardware sanity check rather than a differentiator.
static inline float mathArg(float v)
{
    volatile float x = v;
    return x;
}

static void appendSimulationMathCrc(XferCRC &xfer)
{
    Matrix3D matrix;
    Matrix3D factorsMatrix;

    matrix.Set(
        4.1f, 1.2f, 0.3f, 0.4f,
        0.5f, 3.6f, 0.7f, 0.8f,
        0.9f, 1.0f, 2.1f, 1.2f);

    factorsMatrix.Set(
        WWMath::Sin(mathArg(0.7f)) * log10f(mathArg(2.3f)),
        WWMath::Cos(mathArg(1.1f)) * powf(mathArg(1.1f), mathArg(2.0f)),
        tanf(mathArg(0.3f)),
        asinf(mathArg(0.967302263f)),
        acosf(mathArg(0.967302263f)),
        atanf(mathArg(0.967302263f)) * powf(mathArg(1.1f), mathArg(2.0f)),
        atan2f(mathArg(0.4f), mathArg(1.3f)),
        sinhf(mathArg(0.2f)),
        coshf(mathArg(0.4f)) * tanhf(mathArg(0.5f)),
        sqrtf(mathArg(55788.84375f)),
        expf(mathArg(0.1f)) * log10f(mathArg(2.3f)),
        logf(mathArg(1.4f)));

    Matrix3D::Multiply(matrix, factorsMatrix, &matrix);
    matrix.Get_Inverse(matrix);

    xfer.xferMatrix3D(&matrix);
}

UnsignedInt SimulationMathCrc::calculate()
{
    XferCRC xfer;
    xfer.open("SimulationMathCrc");

    setFPMode();

    appendSimulationMathCrc(xfer);

    // GeneralsX @diag Claude 27/07/2026 Restore the ENGINE's canonical FP mode, not FE_DFL_ENV.
    //
    // This previously restored the C default environment, which is not the mode the rest of the
    // engine runs in - setFPMode() is. That was harmless while calculate() had no callers, but it
    // is now invoked during startup, and handing the remainder of init a different rounding /
    // precision environment than it would otherwise have had is exactly the kind of perturbation
    // that would contaminate a determinism measurement. setFPMode() is idempotent (absolute
    // _controlfp on Windows, fesetround on Apple), so calling it here leaves the caller in the
    // state it was already entitled to assume.
    setFPMode();

    xfer.close();

    return xfer.getCRC();
}
