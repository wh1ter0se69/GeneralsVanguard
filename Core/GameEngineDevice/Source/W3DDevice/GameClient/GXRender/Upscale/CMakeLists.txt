# Copyright 2026 GeneralsX contributors
# SPDX-License-Identifier: GPL-3.0-or-later
# gx_streamline: NVIDIA Streamline (DLSS SR) for the native renderer,
# Windows x64 only (PREREG A36). See GxStreamline.h for the licence rules: nothing of NVIDIA's is in this tree.
#
#   standalone:  cmake -S <this dir> -B <build> -G Ninja -DGX_STREAMLINE_SDK=<extracted streamline-sdk-v2.14.1>
#   the test:    gx_dlss_test --sl-dir <that SDK>\bin\x64     (PREREG A36.1)
# Render-only and outside every digested path: nothing here moves the SimID.
cmake_minimum_required(VERSION 3.20)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(gx_streamline LANGUAGES C CXX)
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    if(NOT CMAKE_BUILD_TYPE)
        set(CMAKE_BUILD_TYPE RelWithDebInfo)
    endif()
    add_subdirectory(../Vk gx_vkctx)
    set(GX_STREAMLINE_STANDALONE ON)
endif()

if(NOT (WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8))
    message(FATAL_ERROR "gx_streamline: Streamline is Windows x64 only")
endif()

# Headers come from the user's Streamline SDK at build time (MIT); never copied into the repo.
set(GX_STREAMLINE_SDK "" CACHE PATH "Extracted NVIDIA Streamline SDK (the folder holding include/ and bin/x64/)")
find_path(GX_STREAMLINE_INCLUDE_DIR sl.h HINTS "${GX_STREAMLINE_SDK}/include" NO_DEFAULT_PATH)
if(NOT GX_STREAMLINE_INCLUDE_DIR)
    message(FATAL_ERROR "gx_streamline: no sl.h - pass -DGX_STREAMLINE_SDK=<extracted Streamline SDK>")
endif()

add_library(gx_streamline STATIC GxStreamline.h GxStreamline.cpp)
target_include_directories(gx_streamline PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}" PRIVATE "${GX_STREAMLINE_INCLUDE_DIR}")
target_compile_definitions(gx_streamline PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(gx_streamline PUBLIC gx_vkctx)

option(GX_STREAMLINE_TESTS "Build gx_dlss_test (PREREG A36.1)" ${GX_STREAMLINE_STANDALONE})
if(GX_STREAMLINE_TESTS)
    add_executable(gx_dlss_test tests/gx_dlss_test.cpp)
    target_compile_definitions(gx_dlss_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(gx_dlss_test PRIVATE gx_streamline)
endif()
