# GeneralsX: VK_LAYER_GX_queue_census: TEST-ONLY Vulkan layer + its selftest (PREREG A39a,
# ). Standalone, never part of the game build, outside every digested path.
#
#   cmake -S <this dir> -B <build> -G Ninja [-DGX_VULKAN_INCLUDE_DIR=<dir holding vulkan/vk_layer.h>]
#   cmake --build <build>
#   <build>/census_selftest control|overlap|plant      each exits 0 when the census reports what that mode does
#   python3 census_verdict.py <summary>                 C1/C2/C3 verdict on a census summary (exit 0 = all PASS)
# In a game run: VK_ADD_LAYER_PATH=<build> VK_INSTANCE_LAYERS=VK_LAYER_GX_queue_census GX_VK_CENSUS=<summary file>
cmake_minimum_required(VERSION 3.20)
project(gx_queue_census LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

get_filename_component(GX_CENSUS_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../../../.." ABSOLUTE)
file(GLOB GX_CENSUS_HOME_SDKS "$ENV{HOME}/VulkanSDK/*/macOS/include")
find_path(GX_VULKAN_INCLUDE_DIR vulkan/vk_layer.h
    HINTS
        "${GX_CENSUS_REPO_ROOT}/references/fbraz3-dxvk/include/vulkan/include"
        "$ENV{VULKAN_SDK}/include"
        ${GX_CENSUS_HOME_SDKS})
if(NOT GX_VULKAN_INCLUDE_DIR)
    message(FATAL_ERROR "gx_queue_census: no vulkan/vk_layer.h - pass -DGX_VULKAN_INCLUDE_DIR=<dir>")
endif()
message(STATUS "gx_queue_census: Vulkan headers from ${GX_VULKAN_INCLUDE_DIR}")

add_library(VkLayer_gx_queue_census SHARED gx_queue_census.cpp)
if(WIN32)
    target_sources(VkLayer_gx_queue_census PRIVATE VkLayer_gx_queue_census.def)	# the loader's entry points (see the .cpp)
endif()
target_include_directories(VkLayer_gx_queue_census PRIVATE "${GX_VULKAN_INCLUDE_DIR}")
set_target_properties(VkLayer_gx_queue_census PROPERTIES CXX_VISIBILITY_PRESET hidden)
if(WIN32)
    target_compile_definitions(VkLayer_gx_queue_census PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
else()
    target_link_libraries(VkLayer_gx_queue_census PRIVATE dl pthread)
endif()

# The manifest sits next to the library and names it relatively, so VK_ADD_LAYER_PATH=<that folder> is all a run needs.
file(GENERATE OUTPUT "$<TARGET_FILE_DIR:VkLayer_gx_queue_census>/VkLayer_gx_queue_census.json" CONTENT
"{
    \"file_format_version\": \"1.2.0\",
    \"layer\": {
        \"name\": \"VK_LAYER_GX_queue_census\",
        \"type\": \"GLOBAL\",
        \"library_path\": \"./$<TARGET_FILE_NAME:VkLayer_gx_queue_census>\",
        \"api_version\": \"1.3.0\",
        \"implementation_version\": \"1\",
        \"description\": \"GeneralsX TEST-ONLY queue census (A39a)\",
        \"functions\": { \"vkNegotiateLoaderLayerInterfaceVersion\": \"vkNegotiateLoaderLayerInterfaceVersion\" }
    }
}
")

add_executable(census_selftest census_selftest.cpp)
target_include_directories(census_selftest PRIVATE "${GX_VULKAN_INCLUDE_DIR}")
target_compile_definitions(census_selftest PRIVATE VK_NO_PROTOTYPES)
if(WIN32)
    target_compile_definitions(census_selftest PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
else()
    target_link_libraries(census_selftest PRIVATE dl pthread)
endif()
add_dependencies(census_selftest VkLayer_gx_queue_census)
