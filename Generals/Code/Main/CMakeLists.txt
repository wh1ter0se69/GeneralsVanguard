add_executable(g_generals WIN32)

# Use a binary name that doesn't conflict with original game.
# GeneralsX: Linux branding update
if("${CMAKE_SYSTEM}" MATCHES "Windows")
    set_target_properties(g_generals PROPERTIES OUTPUT_NAME "generalsv${RTS_BUILD_OUTPUT_SUFFIX}")
else()
    # GeneralsX: Linux branding update
    set_target_properties(g_generals PROPERTIES OUTPUT_NAME GeneralsX)
endif()

# GeneralsX: Windows-specific libraries
if(WIN32)
    target_link_libraries(g_generals PRIVATE
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
    # GeneralsX: d3d8.lib exists only as the x86 archive bundled with
    # min-dx8-sdk; the Windows SDK has never shipped one and a x64 link of it is LNK1112. Nothing
    # needs it either way - dx8wrapper.cpp:581 loads D3D8.DLL and resolves Direct3DCreate8 with
    # GetProcAddress at runtime. See the x64 block in cmake/dx8.cmake.
    if(CMAKE_SIZEOF_VOID_P EQUAL 4)
        target_link_libraries(g_generals PRIVATE d3d8)
    endif()
endif()

# Cross-platform core libraries
# GeneralsX: upstream PR #3070 (36ab560dc) links core_debug and
# core_profile_legacy into the Generals executable so it matches the Zero Hour twin, which
# already links both. core_debug goes in the WIN32 block above instead of here because
# Core/Libraries/Source/debug/CMakeLists.txt wraps add_library(core_debug ...) in if(WIN32),
# so the target does not exist on the macOS/Linux presets.
target_link_libraries(g_generals PRIVATE
    core_profile_legacy
    d3dx8
    g_gameengine
    g_gameenginedevice
    gi_always
)

# Linux SDL3 windowing/input
# GeneralsX:
if(SAGE_USE_SDL3)
    target_link_libraries(g_generals PRIVATE sdl3lib)
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
if (IS_VS6_BUILD)
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/BuildVersion.h
"#pragma once

#define VERSION_MAJOR 1
#define VERSION_MINOR 7
#define VERSION_BUILDNUM 601
"
)
else()
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/BuildVersion.h
"#pragma once

#define VERSION_MAJOR 1
#define VERSION_MINOR 8
#define VERSION_BUILDNUM 601
"
)
endif()

if(MSVC)
    target_link_options(g_generals PRIVATE "/NODEFAULTLIB:libci.lib")
endif()

# GeneralsX: Drop code and data nothing can reach from the game executable (cleanup Batch 2,
# measured on macOS: about -2 MB per game). Link options only: no object file changes, and this file is not on the
# SimID digest's build-config list, so the simulation identity cannot move.
# Nothing looks a symbol up in the executable at run time except one: on iOS, SDL's UIKit Vulkan loader falls back to
# dlsym(RTLD_DEFAULT, "vkGetInstanceProcAddr") in the statically linked MoltenVK (cmake/config-build.cmake). -dead_strip
# keeps that symbol exported, because MoltenVK reaches it (measured with nm -gU).
# GeneralsX: NEVER restrict the export list (-exported_symbol / -exported_symbols_list). The game
# replaces the global operator new/delete (GameMemory.cpp), and dyld binds libc++ and the dylibs to that replacement only
# while the executable exports it. An iOS-only `-exported_symbol _vkGetInstanceProcAddr` removed that export, so memory
# allocated by one allocator was freed by the other and the iPad aborted at launch ("pointer being freed was not allocated").
# Windows ships RelWithDebInfo, whose /DEBUG makes the linker default /OPT:NOREF,NOICF, so REF is asked for here. ICF
# stays off: identical-COMDAT folding gives different functions one address, which breaks code that compares them.
# Linux is left out on purpose: without -ffunction-sections (a compile flag, in the digested cmake/) --gc-sections
# measured -81 KB of 232 MB.
if(APPLE)
    target_link_options(g_generals PRIVATE "LINKER:-dead_strip")
    # GeneralsX: Keep the whole set of operator new/delete overloads GameMemory.cpp replaces alive
    # and exported, exactly as before -dead_strip: dyld lets libc++ and the dylibs use a replacement only while the
    # executable exports it, and -dead_strip dropped the overloads the game's own code never calls (measured: macOS kept 4
    # of the 16, iOS 6). -u makes each one a dead-strip root; the cost is about 2 KB. Only with the pool allocator
    # (RTS_GAMEMEMORY_ENABLE, cmake/config-memory.cmake): without it GameMemoryNull.cpp's operators are plain malloc/free,
    # so a mix with the system allocator is harmless, and DISABLE_GAMEMEMORY_NEW_OPERATORS can leave some undefined, which
    # -u would turn into a link error.
    if(RTS_GAMEMEMORY_ENABLE)
        foreach(_gx_alloc_sym
                __Znwm __Znam __ZdlPv __ZdaPv __ZdlPvm __ZdaPvm
                __ZnwmSt11align_val_t __ZnamSt11align_val_t __ZdlPvSt11align_val_t __ZdaPvSt11align_val_t
                __ZdlPvmSt11align_val_t __ZdaPvmSt11align_val_t
                __ZnwmPKci __ZnamPKci __ZdlPvPKci __ZdaPvPKci)
            target_link_options(g_generals PRIVATE "LINKER:-u,${_gx_alloc_sym}")
        endforeach()
    endif()
elseif(MSVC)
    target_link_options(g_generals PRIVATE "$<$<CONFIG:RelWithDebInfo>:/OPT:REF>" "$<$<CONFIG:RelWithDebInfo>:/OPT:NOICF>")
endif()

target_include_directories(g_generals PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_BINARY_DIR}/Code/Libraries
)

target_sources(g_generals PRIVATE
    $<$<BOOL:${WIN32}>:WinMain.cpp>
    $<$<BOOL:${WIN32}>:WinMain.h>
    # Linux SDL3 entry point
    $<$<NOT:$<BOOL:${WIN32}>>:SDL3Main.cpp>
    # Linux stubs
    $<$<NOT:$<BOOL:${WIN32}>>:LinuxStubs.cpp>
)

# RC files are optional for MinGW builds
# Icon: LoadIcon() handles missing resource gracefully (uses default system icon)
# Manifest: DPI awareness metadata (nice to have but not essential)
# TYPELIB: Broken (0 bytes) and causes windres memory exhaustion errors
if(MSVC)
    # VS2005 and later adds default manifest, we need to turn it off to prevent conflict with custom manifest
    if(NOT IS_VS6_BUILD)
        target_link_options(g_generals PRIVATE
            "/MANIFEST:NO"
        )
    endif()

    target_sources(g_generals PRIVATE
        RTS.RC
    )
endif()

# Strip debug symbols to separate file for MinGW Release builds
# This creates generalsv${RTS_BUILD_OUTPUT_SUFFIX}.exe.debug (similar to MSVC .pdb files)
if(MINGW AND COMMAND add_debug_strip_target)
    add_debug_strip_target(g_generals)
endif()
