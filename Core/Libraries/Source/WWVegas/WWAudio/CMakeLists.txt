set(WWAUDIO_SRC
    AABTreeSoundCullClass.h
    AudibleSound.cpp
    AudibleSound.h
    AudioEvents.h
    AudioSaveLoad.cpp
    AudioSaveLoad.h
    FilteredSound.cpp
    FilteredSound.h
    Listener.cpp
    Listener.h
    listenerhandle.cpp
    listenerhandle.h
    LogicalListener.cpp
    LogicalListener.h
    LogicalSound.cpp
    LogicalSound.h
    PriorityVector.h
    sound2dhandle.cpp
    sound2dhandle.h
    Sound3D.cpp
    Sound3D.h
    sound3dhandle.cpp
    sound3dhandle.h
    SoundBuffer.cpp
    SoundBuffer.h
    SoundChunkIDs.h
    SoundCullObj.h
    soundhandle.cpp
    soundhandle.h
    SoundPseudo3D.cpp
    SoundPseudo3D.h
    SoundScene.cpp
    SoundScene.h
    SoundSceneObj.cpp
    SoundSceneObj.h
    soundstreamhandle.cpp
    soundstreamhandle.h
    Threads.cpp
    Threads.h
    Utils.cpp
    Utils.h
    WWAudio.cpp
    WWAudio.h
)

# @todo Promote this to STATIC library when WW3D2 is a STATIC library as well

add_library(corei_wwaudio INTERFACE)

target_sources(corei_wwaudio INTERFACE ${WWAUDIO_SRC})

target_include_directories(corei_wwaudio INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

# GeneralsX @bugfix Claude 27/07/2026
# The non-Windows shim headers now live in compat/ (see compat/mmsystem.h for why). The include
# entry that exposes them is NOT here: this directory is not how the WWAudio path reaches the tree.
# corei_wwaudio has only ~40 consumers, whereas ../CMakeLists.txt lists WWAudio on core_wwcommon,
# which is what puts it on 2190 translation units - including WW3D2's dx8caps.cpp and
# textureloader.cpp, the actual <mmsystem.h> users. compat/ is added next to it there so the two
# entries stay together and cover the same targets.

target_link_libraries(corei_wwaudio INTERFACE
    core_wwcommon
)
