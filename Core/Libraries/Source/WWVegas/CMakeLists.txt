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

# GeneralsX: WWAudio/compat holds stand-ins for Windows SDK headers (currently mmsystem.h) for platforms with no SDK.
# It is separate from WWAudio/ because that directory is on every target's -I line, and on MSVC a /I dir
# out-ranks the SDK include path, so a mmsystem.h there would shadow the real one. Keep this entry next to the
# WWAudio entry above so both reach exactly the same targets.
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
