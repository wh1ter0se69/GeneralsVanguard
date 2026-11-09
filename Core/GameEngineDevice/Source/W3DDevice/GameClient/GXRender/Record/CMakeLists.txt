# GeneralsX @feature E · rw2-F1 11/10/2026 WS-F1: gx_record (GxPacketWriter v1 - one recording thread, the pass graph) and its
# gate test. Render-only and outside every digested path (resources/gitinfo/simsourcedigest_watcher.cmake): never list these
# sources in a WW3D2 CMakeLists. Standalone: cmake -S <this dir> -B <dir> -G Ninja && cmake --build <dir>
#   -> libgx_record.a + gx_record_test (pulls ../Res, ../Pipeline and ../Vk when the parent did not add them).
cmake_minimum_required(VERSION 3.20)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(gx_record LANGUAGES C CXX)
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    if(NOT CMAKE_BUILD_TYPE)
        set(CMAKE_BUILD_TYPE RelWithDebInfo)
    endif()
    set(GX_RECORD_STANDALONE ON)
endif()

if(NOT TARGET gx_res)
    set(GX_RES_TESTS OFF CACHE BOOL "" FORCE)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../Res" "${CMAKE_CURRENT_BINARY_DIR}/res")
endif()
if(NOT TARGET gx_pipeline)
    set(GX_PIPELINE_TESTS OFF CACHE BOOL "" FORCE)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../Pipeline" "${CMAKE_CURRENT_BINARY_DIR}/pipeline")
endif()

add_library(gx_record STATIC
    GxRecordLayout.h
    GxRecordLayout.cpp
    GxSamplers.h
    GxSamplers.cpp
    GxImageTracker.h
    GxImageTracker.cpp
    GxPacketWriter.h
    GxPacketWriter.cpp
    GxShadowCasters.h
    GxShadowCasters.cpp
    GxOrderCheck.h
    GxOrderCheck.cpp
    GxD3D8Translate.h
    GxD3D8Translate.cpp
)
target_include_directories(gx_record PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(gx_record PUBLIC gx_res gx_pipeline)
set_target_properties(gx_record PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(gx_record PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
endif()

option(GX_RECORD_TESTS "Build gx_record_test (the WS-F1 gates and fail arms)" ${GX_RECORD_STANDALONE})
if(GX_RECORD_TESTS)
    add_executable(gx_record_test tests/gx_record_test.cpp tests/gx_cmd_tap.cpp)
    target_include_directories(gx_record_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests/shaders" "${CMAKE_CURRENT_SOURCE_DIR}/../Pipeline/shaders")
    target_link_libraries(gx_record_test PRIVATE gx_record)
    # a log must prove its build (RW2 common part): gx_record_sha.h is regenerated at every build from git (sha + dirty flag),
    # never frozen at configure time
    add_custom_target(gx_record_sha
        COMMAND ${CMAKE_COMMAND} -DSRC=${CMAKE_CURRENT_SOURCE_DIR} -DOUT=${CMAKE_CURRENT_BINARY_DIR}/gx_record_sha.h
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/gx_sha.cmake
        BYPRODUCTS ${CMAKE_CURRENT_BINARY_DIR}/gx_record_sha.h)
    add_dependencies(gx_record_test gx_record_sha)
    # E · rw3-L: the translator's d3d8 -> packet rules, one case per rule + an X- arm per GxTranslateDebug plant
    add_executable(gx_translate_test tests/gx_translate_test.cpp tests/gx_cmd_tap.cpp)
    target_include_directories(gx_translate_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../Pipeline/shaders")
    target_link_libraries(gx_translate_test PRIVATE gx_record)
    target_include_directories(gx_record_test PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
endif()
