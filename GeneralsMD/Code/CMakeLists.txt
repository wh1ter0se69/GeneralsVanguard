# z stands for Zero Hour, i stands for Interface
add_library(zi_gameengine_include INTERFACE)
add_library(zi_libraries_source_wwvegas INTERFACE)
add_library(zi_main INTERFACE)
add_library(zi_always INTERFACE)
add_library(zi_always_no_pch INTERFACE) # Use this for Shared Libs with MFC AFX

target_include_directories(zi_gameengine_include INTERFACE "GameEngine/Include")
target_include_directories(zi_libraries_source_wwvegas INTERFACE "Libraries/Source/WWVegas")
target_include_directories(zi_main INTERFACE "Main")
# GeneralsX @bugfix Claude 27/07/2026 On Windows expose ONLY the d3dx8 headers from CompatLib.
#
# CompatLib/Include holds windows.h, dinput.h, comip.h, comutil.h and friends - a shim layer
# written so the engine can build on Linux/macOS. Its windows.h says it is "only reached if no SDK
# windows.h exists", which is simply not true: this is a -I directory, so the preprocessor searches
# it BEFORE the system paths and the shim always wins. Its _WIN32 arm then routes into
# windows_compat.h, which includes <pthread.h>, <unistd.h> and <sys/time.h> unguarded.
#
# The result on Windows was 5529 compile errors from one cause: the real Windows SDK never got
# included, so GetLastError, FormatMessage, timeGetTime and every Win32 type were undeclared.
#
# Windows already supplies dinput.h, comip.h and comutil.h from the SDK. The only headers genuinely
# missing on a modern Windows install are the d3dx8 ones (D3DX was dropped with the legacy DirectX
# SDK), and those are already written to do the right thing - d3dx8core.h has an explicit
# "#ifdef _WIN32 -> #include <windows.h>" arm expecting the real SDK. So on Windows we stage just
# those four into the build tree and put THAT on the include path.
if(WIN32)
    set(_compatlib_win_inc "${CMAKE_CURRENT_BINARY_DIR}/compatlib-d3dx8-only")
    # Exported so subdirectories added later (GameEngine) reuse the SAME staged directory
    # instead of reaching back into CompatLib/Include and re-shadowing the Windows SDK.
    set(GX_COMPATLIB_INCLUDE_DIR "${_compatlib_win_inc}")
    file(MAKE_DIRECTORY "${_compatlib_win_inc}")
    file(COPY
        "${CMAKE_CURRENT_SOURCE_DIR}/CompatLib/Include/d3dx8.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/CompatLib/Include/d3dx8core.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/CompatLib/Include/d3dx8math.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/CompatLib/Include/d3dx8tex.h"
        DESTINATION "${_compatlib_win_inc}")
    target_include_directories(zi_always INTERFACE "${_compatlib_win_inc}")
else()
    target_include_directories(zi_always INTERFACE "CompatLib/Include")  # TheSuperHackers @build 08/02/2026 Add DirectX 8 compat headers
    set(GX_COMPATLIB_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/CompatLib/Include")
endif()

target_compile_definitions(zi_always INTERFACE
    RTS_ZEROHOUR=1
)
target_compile_definitions(zi_always_no_pch INTERFACE
    RTS_ZEROHOUR=1
)
target_link_libraries(zi_gameengine_include INTERFACE
    corei_gameengine_include
)
target_link_libraries(zi_always INTERFACE
    corei_always # Must stay below so headers from game are included first
)
target_link_libraries(zi_always_no_pch INTERFACE
    corei_always_no_pch # Must stay below so headers from game are included first
)

# CompatLib: Platform compatibility layer (Linux headers, d3d8lib config)
add_subdirectory(CompatLib)

# Contains internal libraries
add_subdirectory(Libraries)

# Base GameEngine library
add_subdirectory(GameEngine)

# Platform specific GameEngine code
add_subdirectory(GameEngineDevice)

# Contains the main game exe target.
add_subdirectory(Main)

if (RTS_BUILD_ZEROHOUR_TOOLS OR RTS_BUILD_ZEROHOUR_EXTRAS)
    add_subdirectory(Tools)
endif()
