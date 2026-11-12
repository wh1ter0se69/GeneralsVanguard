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

// Brightness adjustment via macOS CGSetDisplayTransferByFormula. SDL3 deprecated
// the gamma APIs in 3.0; CoreGraphics still works for the active display.
//
// Range: 0.5 (very dark) .. 2.0 (very bright). Internal counter goes -128..+128
// for parity with GenTool's brightness slider semantics, then maps to a gamma
// curve via gamma = 1.0 + (value / 256.0).

#include "SagePatch/Features.h"
#include "SagePatch/Logger.h"

#include <ApplicationServices/ApplicationServices.h>

namespace sagepatch {

static int g_brightness = 0; // -128 .. +128

static int clampi(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static void applyGamma() {
    float gamma = 1.0f + static_cast<float>(g_brightness) / 256.0f;
    if (gamma < 0.5f) gamma = 0.5f;
    if (gamma > 2.0f) gamma = 2.0f;

    CGDirectDisplayID displays[8];
    uint32_t count = 0;
    if (CGGetActiveDisplayList(8, displays, &count) != kCGErrorSuccess) {
        SAGEPATCH_LOG("Brightness: cannot enumerate displays");
        return;
    }

    for (uint32_t i = 0; i < count; ++i) {
        CGSetDisplayTransferByFormula(
            displays[i],
            0.0f, 1.0f, 1.0f / gamma,
            0.0f, 1.0f, 1.0f / gamma,
            0.0f, 1.0f, 1.0f / gamma);
    }
}

void adjustBrightness(int delta) {
    g_brightness = clampi(g_brightness + delta, -128, +128);
    applyGamma();
    SAGEPATCH_LOG("Brightness: %+d (gamma %.3f)", g_brightness,
                  1.0 + g_brightness / 256.0);
}

}
