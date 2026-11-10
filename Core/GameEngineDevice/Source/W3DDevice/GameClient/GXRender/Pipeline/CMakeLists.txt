# GeneralsX: WS-D: gx_pipeline (PipelineKey + GxPipelineCache) and its gate test.
#
# Standalone (minutes):  cmake -S <this dir> -B <build> -G Ninja [-DGX_VKCTX_DIR=<a gx_vkctx source dir>] && cmake --build <build>
# From the game: Core/GameEngineDevice/CMakeLists.txt adds it next to ../Vk when GX_RENDER_VK=ON. Render-only, outside every
# digested path. The generic shaders are SPIR-V compiled at build time by shaders/regen.sh and committed (no glslang in a
# game build, no runtime GLSL compiler - lead C31). GxGenericSet.inc is generated from the census (scripts/render/dxvk-census).
cmake_minimum_required(VERSION 3.20)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(gx_pipeline LANGUAGES C CXX)
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    if(NOT CMAKE_BUILD_TYPE)
        set(CMAKE_BUILD_TYPE RelWithDebInfo)
    endif()
    set(GX_PIPELINE_STANDALONE ON)
endif()

if(NOT TARGET gx_vkctx)
    set(GX_VKCTX_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../Vk" CACHE PATH "gx_vkctx source directory (WS-B)")
    add_subdirectory("${GX_VKCTX_DIR}" gx_vkctx)
endif()

add_library(gx_pipeline STATIC
    GxPipelineKey.h
    GxPipelineCache.h
    GxPipelineCache.cpp
    GxGenericSet.inc
    GxFFState.h
    GxAlphaToCoverage.h
    GxFFState.cpp
    GxCustomCatalog.h
    GxCustomCatalog.cpp
    GxCustomCatalog.inc
)
target_include_directories(gx_pipeline PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(gx_pipeline PUBLIC gx_vkctx)

option(GX_PIPELINE_TESTS "Build gx_pipecache_test (the WS-D gates and fail arms)" ${GX_PIPELINE_STANDALONE})
if(GX_PIPELINE_TESTS)
    add_executable(gx_pipecache_test tests/gx_pipecache_test.cpp)
    target_link_libraries(gx_pipecache_test PRIVATE gx_pipeline)
    # rw2-D: the FF ubershader gates - layout vs SPIR-V, generic-vs-specialised pixel identity, cold-cache replay.
    # WS-E (rw2-E): the custom-shader catalogue's gates (no GPU).
    add_executable(gx_cs_test tests/gx_cs_test.cpp)
    target_link_libraries(gx_cs_test PRIVATE gx_pipeline)
    # the suite's fail arm: the same tests against a catalogue that trusts file names (must FAIL: ctest-style WILL_FAIL)
    add_executable(gx_cs_test_broken tests/gx_cs_test.cpp GxCustomCatalog.cpp)
    target_compile_definitions(gx_cs_test_broken PRIVATE GX_CS_BROKEN_BY_NAME)
    target_include_directories(gx_cs_test_broken PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    add_executable(gx_ff_test tests/gx_ff_test.cpp)
    target_link_libraries(gx_ff_test PRIVATE gx_pipeline)

    # rw2-D: GxPackFF's key rules, no GPU. Its D3D8 numbers are checked against the headers the game is built with (rw1-C's
    # d3d-values pattern); a worktree has empty submodules: pass -DGX_FF_D3D8_HINT=<dir of d3d8types.h> -DGX_FF_WINDOWS_HINT=<dir of windows_base.h>.
    add_executable(gx_ffstate_test tests/gx_ffstate_test.cpp GxFFState.cpp)
    target_include_directories(gx_ffstate_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    get_filename_component(GX_FF_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../../.." ABSOLUTE)
    find_path(GX_FF_D3D8_DIR d3d8types.h
        HINTS "${GX_FF_REPO_ROOT}/references/fbraz3-dxvk/include/native/directx" ${GX_FF_D3D8_HINT} NO_DEFAULT_PATH)
    find_path(GX_FF_WINDOWS_DIR windows_base.h
        HINTS "${GX_FF_REPO_ROOT}/references/fbraz3-dxvk/include/native/windows" ${GX_FF_WINDOWS_HINT} ${GX_FF_D3D8_HINT} NO_DEFAULT_PATH)
    if(GX_FF_D3D8_DIR AND GX_FF_WINDOWS_DIR)
        target_compile_definitions(gx_ffstate_test PRIVATE GX_FF_HAVE_D3D8_HEADERS)
        target_include_directories(gx_ffstate_test PRIVATE "${GX_FF_WINDOWS_DIR}" "${GX_FF_D3D8_DIR}")
        message(STATUS "gx_pipeline: gx_ffstate_test checks GxFFState.h's D3D8 numbers against ${GX_FF_D3D8_DIR}")
    else()
        message(STATUS "gx_pipeline: no D3D8 headers found - gx_ffstate_test skips the D3D8 number check")
    endif()
endif()
