# GeneralsX @feature C · rw1-C 10/10/2026 WS-C GXRES v0: D3D8-semantics textures, buffers and render targets over VkCtx v0.
# Render-only and outside every digested path (resources/gitinfo/simsourcedigest_watcher.cmake): never list these sources in
# a WW3D2 CMakeLists. Standalone: cmake -S <this dir> -B <dir> -G Ninja && cmake --build <dir>  ->  libgx_res.a + gx_res_test.
cmake_minimum_required(VERSION 3.20)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(gx_res LANGUAGES C CXX)
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    if(NOT CMAKE_BUILD_TYPE)
        set(CMAKE_BUILD_TYPE RelWithDebInfo)
    endif()
    set(GX_RES_STANDALONE ON)
endif()

if(NOT TARGET gx_vkctx)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../Vk" "${CMAKE_CURRENT_BINARY_DIR}/vk")
endif()

add_library(gx_res STATIC
    GxD3D.h
    GxFormat.h
    GxFormat.cpp
    GxDecode.h
    GxDecode.cpp
    GxResources.h
    GxResources.cpp
    GxBuffer.h
    GxBuffer.cpp
    GxTexture.h
    GxTexture.cpp
    GxRenderTarget.h
    GxRenderTarget.cpp
)
target_include_directories(gx_res PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(gx_res PUBLIC gx_vkctx)
set_target_properties(gx_res PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(gx_res PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
endif()

option(GX_RES_TESTS "Build gx_res_test (the WS-C gates and fail arms)" ${GX_RES_STANDALONE})
if(GX_RES_TESTS)
    add_executable(gx_res_test tests/gx_res_test.cpp tests/gx_res_d3dvalues.cpp)
    target_link_libraries(gx_res_test PRIVATE gx_res)
    # Provenance (rw2-C): every log's first line names the commit it was built from. Regenerated on EVERY build (ALL target,
    # copy_if_different), so a rebuild after a commit cannot keep printing the old sha.
    get_filename_component(GX_RES_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../../.." ABSOLUTE)
    add_custom_target(gx_res_build_sha ALL
        COMMAND ${CMAKE_COMMAND} -DREPO=${GX_RES_REPO_ROOT} -DOUT=${CMAKE_CURRENT_BINARY_DIR}/gx_res_build_sha.h
            -P ${CMAKE_CURRENT_SOURCE_DIR}/gx_res_sha.cmake
        BYPRODUCTS ${CMAKE_CURRENT_BINARY_DIR}/gx_res_build_sha.h
        COMMENT "gx_res: build sha")
    add_dependencies(gx_res_test gx_res_build_sha)
    target_include_directories(gx_res_test PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
    # The d3d-values arm checks GxD3D.h against the D3D8 headers the game is built with (the DXVK fork's native headers
    # on macOS, the steamrt tarball's flat include/dxvk on Linux). Compiled in its own translation unit, away from Vulkan.
    # A worktree has empty submodules: pass -DGX_RES_D3D8_HINT=<dir of d3d8types.h> -DGX_RES_WINDOWS_HINT=<dir of windows_base.h>.
    find_path(GX_RES_D3D8_DIR d3d8types.h
        HINTS "${GX_RES_REPO_ROOT}/references/fbraz3-dxvk/include/native/directx" ${GX_RES_D3D8_HINT} NO_DEFAULT_PATH)
    find_path(GX_RES_WINDOWS_DIR windows_base.h
        HINTS "${GX_RES_REPO_ROOT}/references/fbraz3-dxvk/include/native/windows" ${GX_RES_WINDOWS_HINT} ${GX_RES_D3D8_HINT} NO_DEFAULT_PATH)
    if(GX_RES_D3D8_DIR AND GX_RES_WINDOWS_DIR)
        set_source_files_properties(tests/gx_res_d3dvalues.cpp PROPERTIES
            COMPILE_DEFINITIONS GX_RES_HAVE_D3D8_HEADERS
            INCLUDE_DIRECTORIES "${GX_RES_WINDOWS_DIR};${GX_RES_D3D8_DIR}")
        message(STATUS "gx_res: d3d-values arm checks against ${GX_RES_D3D8_DIR}")
    else()
        message(STATUS "gx_res: no D3D8 headers found - the d3d-values arm reports SKIPPED")
    endif()
    # gx_res_mem (rw2-C): the phys_footprint A/B, native GXRES vs classic DXVK d3d8 on one texture set. macOS only
    # (task_vm_info, footprint); its classic half needs the D3D8 headers and dlopens SDL3 + DXVK at run time (links neither).
    if(APPLE AND NOT IOS AND GX_RES_D3D8_DIR AND GX_RES_WINDOWS_DIR)
        add_executable(gx_res_mem tests/gx_res_mem.cpp tests/gx_res_mem_d3d8.cpp)
        target_link_libraries(gx_res_mem PRIVATE gx_res ${CMAKE_DL_LIBS})
        add_dependencies(gx_res_mem gx_res_build_sha)
        target_include_directories(gx_res_mem PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
        set_source_files_properties(tests/gx_res_mem_d3d8.cpp PROPERTIES
            INCLUDE_DIRECTORIES "${GX_RES_WINDOWS_DIR};${GX_RES_D3D8_DIR}")
    endif()
endif()
