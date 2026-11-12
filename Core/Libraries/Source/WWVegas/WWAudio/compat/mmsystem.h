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

/*
** mmsystem.h - Windows Multimedia System stub
**
** TheSuperHackers @build 15/12/2024
** Minimal stub for Linux builds. Download.cpp includes this on Windows only.
** For Linux builds with OpenAL, this provides empty type definitions.
*/

// GeneralsX:
// This stub lives in WWAudio/compat/ rather than WWAudio/ because WWAudio/ is on the -I path of
// almost every translation unit in the tree (corei_wwaudio exports it INTERFACE-wide). A file
// literally named mmsystem.h sitting there out-ranks the Windows SDK's <mmsystem.h> on MSVC, so on
// Windows every #include <mmsystem.h> resolved to this empty file - killing LPHWAVEOUT/LPWAVEFORMAT
// for the Miles stub (mss.h:205) and, through it, all of WWAudio.h/AudibleSound.h/Utils.h.
// WWAudio/CMakeLists.txt now exports this directory only when NOT WIN32, so Windows gets the real
// SDK header and non-Windows targets resolve exactly as before.

#pragma once

#if defined(SAGE_USE_OPENAL) && !defined(_WIN32)
    #ifndef _MMSYSTEM_H_
    #define _MMSYSTEM_H_

    // Empty stub - multimedia functions are not used in Linux builds
    // Download.cpp only uses this header preparation, actual multimedia
    // operations are stubbed or not called in Linux builds.

    #endif
#endif
