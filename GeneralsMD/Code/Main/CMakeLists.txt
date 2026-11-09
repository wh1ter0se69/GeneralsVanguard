add_executable(z_generals WIN32)

# Use a binary name that doesn't conflict with original game.
# GeneralsX @build BenderAI 15/02/2026 Linux branding update
if("${CMAKE_SYSTEM}" MATCHES "Windows")
    set_target_properties(z_generals PROPERTIES OUTPUT_NAME "generalszh${RTS_BUILD_OUTPUT_SUFFIX}")
else()
    # GeneralsX @build BenderAI 15/02/2026 Linux branding update
    set_target_properties(z_generals PROPERTIES OUTPUT_NAME GeneralsXZH)
endif()

# GeneralsX @build BenderAI 13/02/2026 Windows-specific libraries
if(WIN32)
    target_link_libraries(z_generals PRIVATE
        binkloader
        comctl32
        core_debug
        dinput8
        dxguid
        imm32
        milesloader
        vfw32
        winmm
    )
    # GeneralsX @bugfix Claude 27/07/2026 d3d8.lib exists only as the x86 archive bundled with
    # min-dx8-sdk; the Windows SDK has never shipped one and a x64 link of it is LNK1112. Nothing
    # needs it either way - dx8wrapper.cpp:581 loads D3D8.DLL and resolves Direct3DCreate8 with
    # GetProcAddress at runtime. See the x64 block in cmake/dx8.cmake.
    if(CMAKE_SIZEOF_VOID_P EQUAL 4)
        target_link_libraries(z_generals PRIVATE d3d8)
    endif()
endif()

# Cross-platform core libraries
target_link_libraries(z_generals PRIVATE
    core_profile_legacy
    d3dx8
    z_gameengine
    z_gameenginedevice
    zi_always
)

# Linux SDL3 windowing/input (Phase 1)
# GeneralsX @build BenderAI 11/02/2026
if(SAGE_USE_SDL3)
    target_link_libraries(z_generals PRIVATE sdl3lib)
endif()

# TODO Originally referred to build host and user, replace with git info perhaps?
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/GeneratedVersion.h
"#pragma once

#define VERSION_LOCALBUILDNUM 0
#define VERSION_BUILDUSER \"\"
#define VERSION_BUILDLOC \"\"
"
)

# Based on original binary values for these variables.
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/BuildVersion.h
"#pragma once

#define VERSION_MAJOR 1
#define VERSION_MINOR 4
#define VERSION_BUILDNUM 601
"
)

if(MSVC)
    target_link_options(z_generals PRIVATE "/NODEFAULTLIB:libci.lib")
endif()

# GeneralsX @build Claude 29/10/2026 Drop code and data nothing can reach from the game executable (cleanup Batch 2,
# measured on macOS: about -2 MB per game). Link options only: no object file changes, and this file is not on the
# SimID digest's build-config list, so the simulation identity cannot move.
# Nothing looks a symbol up in the executable at run time except one: on iOS, SDL's UIKit Vulkan loader falls back to
# dlsym(RTLD_DEFAULT, "vkGetInstanceProcAddr") in the statically linked MoltenVK (cmake/config-build.cmake). -dead_strip
# keeps that symbol exported, because MoltenVK reaches it (measured with nm -gU).
# GeneralsX @bugfix Claude 29/10/2026 NEVER restrict the export list (-exported_symbol / -exported_symbols_list). The game
# replaces the global operator new/delete (GameMemory.cpp), and dyld binds libc++ and the dylibs to that replacement only
# while the executable exports it. An iOS-only `-exported_symbol _vkGetInstanceProcAddr` removed that export, so memory
# allocated by one allocator was freed by the other and the iPad aborted at launch ("pointer being freed was not allocated").
# Windows ships RelWithDebInfo, whose /DEBUG makes the linker default /OPT:NOREF,NOICF, so REF is asked for here. ICF
# stays off: identical-COMDAT folding gives different functions one address, which breaks code that compares them.
# Linux is left out on purpose: without -ffunction-sections (a compile flag, in the digested cmake/) --gc-sections
# measured -81 KB of 232 MB.
if(APPLE)
    target_link_options(z_generals PRIVATE "LINKER:-dead_strip")
    # GeneralsX @bugfix Claude 29/10/2026 Keep the whole set of operator new/delete overloads GameMemory.cpp replaces alive
    # and exported, exactly as before -dead_strip: dyld lets libc++ and the dylibs use a replacement only while the
    # executable exports it, and -dead_strip dropped the overloads the game's own code never calls (measured: macOS kept 4
    # of the 16, iOS 6). -u makes each one a dead-strip root; the cost is about 2 KB. Only with the pool allocator
    # (RTS_GAMEMEMORY_ENABLE, cmake/config-memory.cmake): without it GameMemoryNull.cpp's operators are plain malloc/free,
    # so a mix with the system allocator is harmless, and DISABLE_GAMEMEMORY_NEW_OPERATORS can leave some undefined, which
    # -u would turn into a link error (Codex cleanup-b2fix-r2).
    if(RTS_GAMEMEMORY_ENABLE)
        foreach(_gx_alloc_sym
                __Znwm __Znam __ZdlPv __ZdaPv __ZdlPvm __ZdaPvm
                __ZnwmSt11align_val_t __ZnamSt11align_val_t __ZdlPvSt11align_val_t __ZdaPvSt11align_val_t
                __ZdlPvmSt11align_val_t __ZdaPvmSt11align_val_t
                __ZnwmPKci __ZnamPKci __ZdlPvPKci __ZdaPvPKci)
            target_link_options(z_generals PRIVATE "LINKER:-u,${_gx_alloc_sym}")
        endforeach()
    endif()
elseif(MSVC)
    target_link_options(z_generals PRIVATE "$<$<CONFIG:RelWithDebInfo>:/OPT:REF>" "$<$<CONFIG:RelWithDebInfo>:/OPT:NOICF>")
endif()

target_include_directories(z_generals PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}
)

target_sources(z_generals PRIVATE
    # GeneralsX @feature Claude 17/08/2026 Filtered, rotated, capped stderr mirror so a
    # desync's [CRC] report survives on disk. Shared by both entry points; the policy must
    # be one copy, because "which lines survive the cap" is a correctness property.
    GXStderrLog.cpp
    GXStderrLog.h
    # GeneralsX @feature Claude 25/09/2026 One readable crash log per crash, <userdata>/CrashLogs/,
    # every platform. Tested alone by scripts/test/crash-log-test.sh.
    GXCrashLog.cpp
    GXCrashLog.h
    $<$<BOOL:${WIN32}>:WinMain.cpp>
    $<$<BOOL:${WIN32}>:WinMain.h>
    # GeneralsX @feature Claude 23/09/2026 The one-time move into Documents\GeneralsX\GeneralsZH.
    # Windows only: it is the only platform whose user-data folder moved. Its logic is tested on
    # any host by scripts/test/userdata-migrate-test.cpp.
    $<$<BOOL:${WIN32}>:GXUserDataMigrate.cpp>
    $<$<BOOL:${WIN32}>:GXUserDataMigrate.h>
    # GeneralsX @feature Claude 02/10/2026 Prefer the V-cache CCD on X3D CPUs (GXCpuSets.h). Windows only; its
    # decision is tested on any host by scripts/test/x3d-cpusets-test.cpp.
    $<$<BOOL:${WIN32}>:GXCpuSets.cpp>
    $<$<BOOL:${WIN32}>:GXCpuSets.h>
    # Linux SDL3 entry point (Phase 1.5 - TheSuperHackers @build 10/02/2026)
    $<$<NOT:$<BOOL:${WIN32}>>:SDL3Main.cpp>
    # Linux stubs for P2-P3 platform functions (P2-P3 - TheSuperHackers @build 13/02/2026 BenderAI Session 37)
    $<$<NOT:$<BOOL:${WIN32}>>:LinuxStubs.cpp>
)

# RC files optional for MinGW builds
# Icon: LoadIcon() handles missing resource gracefully (uses default system icon)
# Manifest: DPI awareness metadata (nice to have but not essential)
if(MSVC)
    # VS2005 and later adds default manifest, we need to turn it off to prevent conflict with custom manifest
    if(NOT IS_VS6_BUILD)
        target_link_options(z_generals PRIVATE
            "/MANIFEST:NO"
        )
    endif()

    target_sources(z_generals PRIVATE
        RTS.RC
    )
endif()

# Strip debug symbols to separate file for MinGW Release builds
# This creates generalszh${RTS_BUILD_OUTPUT_SUFFIX}.exe.debug (similar to MSVC .pdb files)
if(MINGW AND COMMAND add_debug_strip_target)
    add_debug_strip_target(z_generals)
endif()
