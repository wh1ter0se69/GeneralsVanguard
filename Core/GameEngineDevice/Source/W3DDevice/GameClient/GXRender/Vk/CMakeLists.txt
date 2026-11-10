# GeneralsX: WS-B: gx_vkctx, the native renderer's Vulkan device layer, and its self-test.
#
# Two ways in, one file:
#   * standalone (minutes, no game configure):  cmake -S <this dir> -B <build> -G Ninja && cmake --build <build>
#   * from the game: Core/GameEngineDevice/CMakeLists.txt adds this directory when GX_RENDER_VK=ON (default OFF until the
#     session switch, WS-J, wires a backend to it).
# Render-only and outside every digested path (resources/gitinfo/simsourcedigest_watcher.cmake): nothing here moves the
# SimID. No network fetch: volk and VMA are vendored in ../third_party (SOURCE.txt). Nothing links a Vulkan loader -
# VkCtx resolves vkGetInstanceProcAddr at run time from the library the process already uses.
cmake_minimum_required(VERSION 3.20)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(gx_vkctx LANGUAGES C CXX)
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    if(NOT CMAKE_BUILD_TYPE)
        set(CMAKE_BUILD_TYPE RelWithDebInfo)
    endif()
    set(GX_VKCTX_STANDALONE ON)
endif()

get_filename_component(GX_VKCTX_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../../.." ABSOLUTE)
set(GX_VKCTX_THIRD_PARTY "${CMAKE_CURRENT_SOURCE_DIR}/../third_party")

# Vulkan headers only (no library). Order: an explicit GX_VULKAN_INCLUDE_DIR, the DXVK fork's own headers (the ones DXVK
# compiles against in this repo), $VULKAN_SDK, the LunarG SDK under ~/VulkanSDK (macOS), then system paths.
file(GLOB GX_VKCTX_HOME_SDKS "$ENV{HOME}/VulkanSDK/*/macOS/include")
list(SORT GX_VKCTX_HOME_SDKS)
list(REVERSE GX_VKCTX_HOME_SDKS)
# iOS only: CMake's Darwin platform sets CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY for iOS, which hides these
# host-side copies; Vulkan headers are target-independent there. Every other platform keeps its toolchain's root policy (MinGW's ONLY).
set(GX_VKCTX_FIND_ROOT)
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set(GX_VKCTX_FIND_ROOT NO_CMAKE_FIND_ROOT_PATH)
endif()
find_path(GX_VULKAN_INCLUDE_DIR vulkan/vulkan_core.h
    HINTS
        "${GX_VKCTX_REPO_ROOT}/references/fbraz3-dxvk/include/vulkan/include"
        "$ENV{VULKAN_SDK}/include"
        ${GX_VKCTX_HOME_SDKS}
    ${GX_VKCTX_FIND_ROOT}
)
if(NOT GX_VULKAN_INCLUDE_DIR)
    # macOS and Windows x64 compile the native renderer in BY DEFAULT now (GX_RENDER_NATIVE), so a
    # fresh checkout lands here unless the DXVK fork's Vulkan-Headers are checked out - say how, and how to build without it.
    message(FATAL_ERROR "gx_vkctx: no Vulkan headers (vulkan/vulkan_core.h). Run `git submodule update --init --recursive "
                        "references/fbraz3-dxvk`, set VULKAN_SDK, or pass -DGX_VULKAN_INCLUDE_DIR=<dir>. In the game, -DGX_RENDER_NATIVE=OFF "
                        "-DGX_RENDER_VK=OFF builds without the native renderer (classic only; both: the cache keeps GX_RENDER_VK ON).")
endif()
message(STATUS "gx_vkctx: Vulkan headers from ${GX_VULKAN_INCLUDE_DIR}")

add_library(gx_vkctx STATIC
    VkCommon.h
    VkCtx.h
    VkCtx.cpp
    VkFrameRing.h
    VkFrameRing.cpp
    VkGpuTimer.h
    VkGpuTimer.cpp
    VkUploadRing.h
    VkUploadRing.cpp
    VkSwapchain.h
    VkSwapchain.cpp
    VkWsi.h
    VkMemAlloc.cpp
    ${GX_VKCTX_THIRD_PARTY}/volk/volk.c
    ${GX_VKCTX_THIRD_PARTY}/volk/volk.h
    ${GX_VKCTX_THIRD_PARTY}/vma/vk_mem_alloc.h
)
target_include_directories(gx_vkctx PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${GX_VKCTX_THIRD_PARTY}/volk"
    "${GX_VKCTX_THIRD_PARTY}/vma"
    "${GX_VULKAN_INCLUDE_DIR}"
)
# VK_NO_PROTOTYPES: every entry point comes from volk. Beta extensions: VK_KHR_portability_subset (MoltenVK).
target_compile_definitions(gx_vkctx PUBLIC VK_NO_PROTOTYPES VK_ENABLE_BETA_EXTENSIONS)
# GeneralsX: iOS: the game links the static libMoltenVK.a, whose vk* FUNCTIONS share the
# names of C volk's global pointers. volk_ns.cpp compiles volk as C++ in namespace volk instead (volk.h: C++ only, and every
# includer must see the define, hence PUBLIC). Every other platform keeps volk.c as shipped.
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set_source_files_properties(${GX_VKCTX_THIRD_PARTY}/volk/volk.c PROPERTIES HEADER_FILE_ONLY ON)
    target_sources(gx_vkctx PRIVATE volk_ns.cpp)
    target_compile_definitions(gx_vkctx PUBLIC VOLK_NAMESPACE)
endif()
if(WIN32)
    # GeneralsX: PRIVATE: as PUBLIC these reached every game source through
    # corei_gameenginedevice_public -> gx_device (NOMINMAX is deliberately off for the game, WWVegas/CMakeLists.txt:5), and with
    # VK_USE_PLATFORM_WIN32_KHR every includer of volk.h dragged in <windows.h>. Only VkWsiWin32.cpp calls the Win32 surface
    # functions (VkWsi.h declares them with void* handles); volk is used without its device/instance tables, so the other TUs
    # seeing volk.h without the platform define is safe.
    target_compile_definitions(gx_vkctx PRIVATE VK_USE_PLATFORM_WIN32_KHR NOMINMAX WIN32_LEAN_AND_MEAN)
    target_sources(gx_vkctx PRIVATE VkWsiWin32.cpp)
endif()
# SDL3 WSI (macOS, Linux, iOS): the game's own SDL3 target when built inside the game, else an installed SDL3.
if(NOT TARGET SDL3::SDL3)
    find_package(SDL3 CONFIG QUIET)
endif()
if(TARGET SDL3::SDL3)
    target_sources(gx_vkctx PRIVATE VkWsiSdl3.cpp)
    target_compile_definitions(gx_vkctx PUBLIC GX_VKCTX_HAS_SDL3)
    target_link_libraries(gx_vkctx PUBLIC SDL3::SDL3)
    message(STATUS "gx_vkctx: SDL3 WSI on")
endif()
set_target_properties(gx_vkctx PROPERTIES POSITION_INDEPENDENT_CODE ON)
# Third-party code compiles as shipped; its warnings are not ours to fix.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    set_source_files_properties(VkMemAlloc.cpp ${GX_VKCTX_THIRD_PARTY}/volk/volk.c volk_ns.cpp PROPERTIES COMPILE_OPTIONS "-w")
elseif(MSVC)
    set_source_files_properties(VkMemAlloc.cpp ${GX_VKCTX_THIRD_PARTY}/volk/volk.c PROPERTIES COMPILE_OPTIONS "/w")
endif()
if(UNIX)
    target_link_libraries(gx_vkctx PUBLIC ${CMAKE_DL_LIBS})
    find_package(Threads REQUIRED)
    target_link_libraries(gx_vkctx PUBLIC Threads::Threads)
endif()

option(GX_VKCTX_TESTS "Build gx_vkctx_test (the WS-B gates and fail arms)" ${GX_VKCTX_STANDALONE})
if(GX_VKCTX_TESTS)
    add_executable(gx_vkctx_test tests/gx_vkctx_test.cpp)
    target_link_libraries(gx_vkctx_test PRIVATE gx_vkctx)
    if(TARGET SDL3::SDL3)
        add_executable(gx_vkctx_wsi_test tests/gx_vkctx_wsi_test.cpp)
        target_link_libraries(gx_vkctx_wsi_test PRIVATE gx_vkctx)
    endif()
endif()
