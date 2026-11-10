# GeneralsX: L1 in the game: gx_device (the native d3d8 device) and
# gx_native_test. Standalone: cmake -S <this dir> -B <build> -G Ninja -DGX_D3D8_INCLUDE=<DXVK include/native>. From the game:
# Core/GameEngineDevice/CMakeLists.txt adds it when GX_RENDER_NATIVE=ON. Render-only, outside every digested path.
cmake_minimum_required(VERSION 3.20)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(gx_device LANGUAGES C CXX)
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    if(NOT CMAKE_BUILD_TYPE)
        set(CMAKE_BUILD_TYPE RelWithDebInfo)
    endif()
    set(GX_DEVICE_STANDALONE ON)
endif()
if(NOT GX_D3D8_INCLUDE AND NOT WIN32)
    get_filename_component(_gxroot "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../../.." ABSOLUTE)
    set(GX_D3D8_INCLUDE "${_gxroot}/references/fbraz3-dxvk/include/native" CACHE PATH "DXVK's include/native (windows/ and directx/)")
endif()
if(NOT TARGET gx_record)
    set(GX_RECORD_TESTS OFF CACHE BOOL "" FORCE)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../Record" "${CMAKE_CURRENT_BINARY_DIR}/record")
endif()
add_library(gx_device STATIC
    GxNativeCs.h
    GxNativeCs.cpp
    GxNativeDevice.h
    GxNativeDevice.cpp
    GxNativeResources.cpp
    GxNativeConfig.h
    GxNativeConfig.cpp
    GxNativePresent.h
    GxNativePresent.cpp
    GxFgUiAlpha.cpp
    GxMotion.h
    GxMotion.cpp
    GxMotionFrame.h
    GxNativeD3D8.h
    GxNativeD3D8.cpp
    GxNativeWorldPass.cpp
    GxSceneCopy.h
    GxSceneCopy.cpp
    GxNativeSceneTap.cpp
    GxNativeDlss.cpp
    GxFsr1.h
    GxFsr1.cpp
    GxNativeLoaderDirect.h
    GxNativeLoaderDirect.cpp
)
target_include_directories(gx_device PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
if(WIN32)
    # GeneralsX: Windows: the real SDK <windows.h> plus DirectX 8 headers - the game's
    # d3d8lib (min-dx8-sdk) inside the game, or -DGX_D3D8_INCLUDE=<min-dx8-sdk dir> standalone. DXVK's include/native/windows
    # shim must never be on this path: it shadows the SDK.
    if(TARGET d3d8lib)
        target_link_libraries(gx_device PRIVATE d3d8lib)
    elseif(GX_D3D8_INCLUDE)
        target_include_directories(gx_device PUBLIC "${GX_D3D8_INCLUDE}")
    else()
        message(FATAL_ERROR "gx_device on Windows: pass -DGX_D3D8_INCLUDE=<min-dx8-sdk dir> (the DirectX 8 headers)")
    endif()
    target_compile_definitions(gx_device PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
else()
    target_include_directories(gx_device PUBLIC "${GX_D3D8_INCLUDE}" "${GX_D3D8_INCLUDE}/windows" "${GX_D3D8_INCLUDE}/directx")
endif()
target_include_directories(gx_device PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../Pipeline/shaders" "${CMAKE_CURRENT_SOURCE_DIR}/../Pipeline")
# render-hud-seam-1: GxWorldPass.h, the seam's device interface, lives with the game-side headers that call it
target_include_directories(gx_device PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../Include/W3DDevice/GameClient")
target_link_libraries(gx_device PUBLIC gx_record)
set_target_properties(gx_device PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(gx_device PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
endif()
option(GX_DEVICE_TESTS "Build gx_native_test" ${GX_DEVICE_STANDALONE})
if(GX_DEVICE_TESTS)
    add_executable(gx_native_test tests/gx_native_test.cpp ../Record/tests/gx_cmd_tap.cpp)
    target_link_libraries(gx_native_test PRIVATE gx_device)
    add_executable(gx_proxy_test tests/gx_proxy_test.cpp ../Record/tests/gx_cmd_tap.cpp)
    target_link_libraries(gx_proxy_test PRIVATE gx_device)
    if(WIN32)
        # The tests see the real SDK <windows.h> through <d3d8.h>; gx_device's NOMINMAX is PRIVATE, so give the tests their own
        # (std::max / numeric_limits<>::min() in gx_native_test.cpp and GxDxvkMath.h are min/max macros otherwise).
        target_compile_definitions(gx_native_test PRIVATE NOMINMAX)
        target_compile_definitions(gx_proxy_test PRIVATE NOMINMAX)
    endif()
    # A23-B arm 26: the front-only results, built here with GX_A23B_FRONT and against the BASE device without it
    # GXGraphics::resolveUpscaler's table (header-only; tests/u2stub stands in for Common/GameDefines.h)
    add_executable(gx_upscaler_resolve_test tests/gx_upscaler_resolve_test.cpp)
    target_include_directories(gx_upscaler_resolve_test PRIVATE tests/u2stub ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../../GameEngine/Include)
    add_executable(gx_cs_front_test tests/gx_cs_front_test.cpp)
    target_link_libraries(gx_cs_front_test PRIVATE gx_device)
    target_compile_definitions(gx_cs_front_test PRIVATE GX_A23B_FRONT=1)
endif()
