/*
** mmsystem.h - Windows Multimedia System stub
**
** TheSuperHackers @build 15/12/2024
** Minimal stub for Linux builds. Download.cpp includes this on Windows only.
** For Linux builds with OpenAL, this provides empty type definitions.
*/

// GeneralsX @bugfix Claude 27/07/2026
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
