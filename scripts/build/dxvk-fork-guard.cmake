# Copyright 2026 GeneralsX contributors
# SPDX-License-Identifier: GPL-3.0-or-later
# dxvk-fork-guard.cmake - the Apple DXVK is the patched fork, or the configure fails.
#
# A FRESH WORKTREE BUILT AN UNPATCHED DXVK, SILENTLY.
#
# cmake/dx8.cmake takes references/fbraz3-dxvk only when that path has a .git, and falls through to a
# GitHub clone at DXVK_REMOTE_REF otherwise - a clone whose wsi_window_sdl3.cpp still calls
# SDL_GetWindowSize, so on a Retina display the picture lands in the top-left quarter of the window and
# the mouse misses by the same factor. `git worktree add` never initialises submodules, so EVERY fresh
# worktree took that branch while its cache said SAGE_DXVK_USE_LOCAL_FORK=ON. Measured 25/09/2026: 19 of
# the 20 worktree DXVK builds on this Mac were the remote clone and failed verify-dxvk-retina-patch.sh,
# and one of them is what the owner was shown over AnyDesk as a "broken build".
#
# Included from CMakeLists.txt around cmake/dx8.cmake, and kept OUT of cmake/ on purpose: resources/
# gitinfo/simsourcedigest_watcher.cmake digests cmake/ (SIMDIGEST_PATHS_BUILD), and which DXVK source an
# Apple build compiles moves no simulation code - a guard there would be a four-platform ship.
#
# Checked by scripts/test/dxvk-fork-guard-test.sh.

# Before dx8.cmake. A fork with NO checkout holds nobody's work, so check out its gitlink, recursively
# (DXVK needs its own nested submodules) - exactly what verify-4platform.sh's local gate already does.
# A checkout that exists ('+' or 'U' included) is somebody's and is never touched here.
function(gx_dxvk_fork_init root)
    set(fork "${root}/references/fbraz3-dxvk")
    if(EXISTS "${fork}/.git")
        return()
    endif()
    message(STATUS "DXVK: ${fork} has no checkout (a fresh git worktree?) - checking out its gitlink; a first run clones it")
    execute_process(COMMAND git -C "${root}" submodule update --init --recursive -- references/fbraz3-dxvk
                    RESULT_VARIABLE rc)
    if(NOT rc EQUAL 0 OR NOT EXISTS "${fork}/.git")
        message(FATAL_ERROR "DXVK: could not check out references/fbraz3-dxvk (git exit ${rc}). Without it cmake/dx8.cmake "
            "builds the UNPATCHED remote DXVK (quarter-size picture on Retina). Run, then configure again:\n"
            "    git submodule update --init --recursive references/fbraz3-dxvk")
    endif()
endfunction()

# After dx8.cmake, with the source it chose. Asks what the build will actually compile, whichever branch of
# dx8.cmake chose it: -DSAGE_DXVK_USE_LOCAL_FORK=OFF, a fork that is still missing, or a fork whose checkout
# lost the fix all land here. dx8.cmake applies Patches/dxvk-ios.patch before this runs, so a fork that only
# needed the patch passes.
function(gx_dxvk_assert_patched root src)
    set(fork "${root}/references/fbraz3-dxvk")
    set(wsi "${src}/src/wsi/sdl3/wsi_window_sdl3.cpp")
    if(NOT src STREQUAL fork)
        message(FATAL_ERROR "DXVK would be built from ${src}, not the patched fork ${fork}. That source lacks the Retina "
            "present-rect fix (quarter-size picture, mouse off by the same factor). Configure with "
            "-DSAGE_DXVK_USE_LOCAL_FORK=ON (every Apple preset sets it) and "
            "git submodule update --init --recursive references/fbraz3-dxvk")
    endif()
    if(NOT EXISTS "${wsi}")
        message(FATAL_ERROR "DXVK: no ${wsi} - cannot show that the DXVK about to be built carries the Retina fix")
    endif()
    file(STRINGS "${wsi}" hits REGEX "SDL_GetWindowSizeInPixels")
    if("${hits}" STREQUAL "")
        message(FATAL_ERROR "DXVK: ${wsi} never calls SDL_GetWindowSizeInPixels, so this checkout lacks the Retina "
            "present-rect fix (Patches/dxvk-ios.patch). Put references/fbraz3-dxvk back at its gitlink: "
            "git submodule update --init --recursive references/fbraz3-dxvk")
    endif()
endfunction()

# Rebuild DXVK when the fork changes. dx8.cmake's ExternalProject has no BUILD_ALWAYS, so a moved fbraz3-dxvk
# gitlink left build/<preset> on the old libdxvk_d3d9. The step depends on the fork's sources (CONFIGURE_DEPENDS).
# An always-dirty input does not work: CMake's restat = 1 leaves the step clean when nothing is written.
function(gx_dxvk_rebuild_on_fork_change src)
    if(NOT TARGET dxvk_macos_build)
        return()
    endif()
    # A checkout left behind a gitlink move (no `git submodule update` in this worktree) would build the OLD fork, and
    # the game takes a DXVK without the warm-up export as "no warm-up", silently. So the checkout must be
    # the commit this tree pins; verify-4platform.sh's local gate already requires the same.
    execute_process(COMMAND git -C "${CMAKE_SOURCE_DIR}" rev-parse "HEAD:references/fbraz3-dxvk"
                    OUTPUT_VARIABLE pinned OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE rc1 ERROR_QUIET)
    execute_process(COMMAND git -C "${src}" rev-parse HEAD
                    OUTPUT_VARIABLE checked_out OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE rc2 ERROR_QUIET)
    if(rc1 EQUAL 0 AND rc2 EQUAL 0 AND NOT pinned STREQUAL checked_out)
        message(FATAL_ERROR "DXVK: references/fbraz3-dxvk is checked out at ${checked_out}, but this commit pins "
            "${pinned}. Building it would ship the wrong DXVK. Run, then configure again:\n"
            "    git submodule update --init --recursive references/fbraz3-dxvk")
    endif()
    # include/ whole: its nested vulkan and spirv submodules are build inputs too.
    file(GLOB_RECURSE fork_inputs CONFIGURE_DEPENDS LIST_DIRECTORIES false
         "${src}/src/*" "${src}/include/*" "${src}/meson.build" "${src}/meson_options.txt")
    ExternalProject_Add_StepDependencies(dxvk_macos_build build ${fork_inputs})
    # And a rebuilt DXVK must reach the build-root copies the bundle prefers (bundle-macos-zh.sh): dx8.cmake's copy rule
    # has only an order-only dependency on the DXVK target, so once a copy existed it was never refreshed (
    # read in build.ninja: "CUSTOM_COMMAND || dxvk_macos_build"). The build step's stamp is touched whenever the step
    # runs, so it is the file-level trigger; copy_if_different keeps an unchanged DXVK from relinking anything.
    ExternalProject_Get_Property(dxvk_macos_build STAMP_DIR)
    add_custom_command(OUTPUT "${CMAKE_BINARY_DIR}/libdxvk_d3d9.0.dylib" APPEND
                       DEPENDS "${STAMP_DIR}/dxvk_macos_build-build")
endfunction()
