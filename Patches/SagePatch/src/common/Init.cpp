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

#include "SagePatch/Hooks.h"
#include "SagePatch/Logger.h"

namespace sagepatch {

#if defined(__APPLE__)
#  define SAGE_LOAD_MECHANISM "DYLD_INSERT_LIBRARIES"
#elif defined(__linux__)
#  define SAGE_LOAD_MECHANISM "LD_PRELOAD"
#else
#  define SAGE_LOAD_MECHANISM "preload"
#endif

__attribute__((constructor))
static void onLoad() {
    SAGEPATCH_LOG("Loaded (version %s) via %s", SAGE_PATCH_VERSION, SAGE_LOAD_MECHANISM);
    init();
}

__attribute__((destructor))
static void onUnload() {
    shutdown();
    SAGEPATCH_LOG("Unloaded");
}

void init() {
    SAGEPATCH_LOG("Hot-keys:");
    SAGEPATCH_LOG("  F11             screenshot (PNG to ~/Pictures/GeneralsX)");
    SAGEPATCH_LOG("  Scroll Lock     toggle cursor lock");
    SAGEPATCH_LOG("  Ctrl+PageUp/Dn  brightness +/-");
    // Was Ctrl+1..5, which ate CREATE_TEAM1..5. See KeyHandler.cpp.
#if defined(__APPLE__)
    SAGEPATCH_LOG("  Cmd+1..5        window position (center / TL / TR / BL / BR)");
#else
    SAGEPATCH_LOG("  Super+1..5      window position (center / TL / TR / BL / BR)");
#endif
}

void shutdown() {}

}
