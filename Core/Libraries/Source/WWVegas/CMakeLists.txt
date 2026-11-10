# Interface libraries to set common defines and includes, avoid duplication later
add_library(core_wwcommon INTERFACE)

target_compile_definitions(core_wwcommon INTERFACE
    #NOMINMAX
    WIN32_LEAN_AND_MEAN
)

# GeneralsX: d3d8lib is INTERFACE (headers only) - safe for all
target_link_libraries(core_wwcommon INTERFACE
    d3d8lib     # INTERFACE target: provides DXVK/DirectX types (DWORD, BOOL, etc.)
    stlport
)

# GeneralsX: Platform-specific multimedia stubs
if(WIN32)
    target_link_libraries(core_wwcommon INTERFACE
        milesloader
    )
endif()

target_include_directories(core_wwcommon INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# GeneralsX:
# WWAudio/compat holds stand-ins for Windows SDK headers (currently mmsystem.h) that satisfy the
# unguarded #include <mmsystem.h> in WW3D2/dx8caps.cpp, WW3D2/textureloader.cpp and W3DMainMenu.cpp
# on platforms with no Windows SDK. They used to sit directly in WWAudio/, but the WWAudio entry
# above lands on the -I line of 2190 translation units, and on MSVC a /I directory out-ranks the SDK
# include path - so a file named mmsystem.h there silently replaced the real <mmsystem.h> for the
# entire Windows build, which is what left Miles' mss.h without LPHWAVEOUT/LPWAVEFORMAT.
# Separating them means Windows now resolves to the genuine SDK header while every other platform
# keeps the shim, and keeping this entry adjacent to the WWAudio entry above guarantees both reach
# exactly the same targets.
if(NOT WIN32)
    target_include_directories(core_wwcommon INTERFACE WWAudio/compat)
endif()

add_subdirectory(WW3D2)
add_subdirectory(WWAudio)
add_subdirectory(WWDebug)
add_subdirectory(WWDownload)
add_subdirectory(WWLib)
add_subdirectory(WWMath)
add_subdirectory(WWSaveLoad)
add_subdirectory(WWStub)

# Helpful interface to bundle the ww modules together.
add_library(core_wwvegas INTERFACE)

target_include_directories(core_wwvegas INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# GeneralsX: Mirror of the core_wwcommon guard above - this list is a
# duplicate of it, so the shim directory has to travel with the WWAudio entry here too.
if(NOT WIN32)
    target_include_directories(core_wwvegas INTERFACE WWAudio/compat)
endif()

target_link_libraries(core_wwvegas INTERFACE
    # core_ww3d2
    # core_wwaudio
    core_wwdebug
    # core_wwdownload
    core_wwlib
    core_wwmath
    core_wwsaveload
)
