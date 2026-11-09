# GeneralsX @build BenderAI 13/02/2026 Disable Miles/Bink for Linux (Windows-only)
set(GAMEENGINEDEVICE_SRC
#    Include/MilesAudioDevice/MilesAudioManager.h
#    Include/VideoDevice/Bink/BinkVideoPlayer.h (see GeneralsMD/Code/GameEngineDevice/Include/W3DDevice/GameClient/W3DGameClient.h createVideoPlayer)
#    Include/W3DDevice/Common/W3DConvert.h
#    Include/W3DDevice/Common/W3DFunctionLexicon.h
#    Include/W3DDevice/Common/W3DModuleFactory.h
    Include/W3DDevice/Common/W3DRadar.h
#    Include/W3DDevice/Common/W3DThingFactory.h
    Include/W3DDevice/GameClient/BaseHeightMap.h
#    Include/W3DDevice/GameClient/camerashakesystem.h
    Include/W3DDevice/GameClient/FlatHeightMap.h
    Include/W3DDevice/GameClient/HeightMap.h
    Include/W3DDevice/GameClient/Module/W3DDebrisDraw.h
    Include/W3DDevice/GameClient/Module/W3DDefaultDraw.h
    Include/W3DDevice/GameClient/Module/W3DDependencyModelDraw.h
    Include/W3DDevice/GameClient/Module/W3DLaserDraw.h
    Include/W3DDevice/GameClient/Module/W3DModelDraw.h
    Include/W3DDevice/GameClient/Module/W3DOverlordAircraftDraw.h
    Include/W3DDevice/GameClient/Module/W3DOverlordTankDraw.h
    Include/W3DDevice/GameClient/Module/W3DOverlordTruckDraw.h
    Include/W3DDevice/GameClient/Module/W3DPoliceCarDraw.h
    Include/W3DDevice/GameClient/Module/W3DProjectileStreamDraw.h
    Include/W3DDevice/GameClient/Module/W3DPropDraw.h
    Include/W3DDevice/GameClient/Module/W3DRopeDraw.h
    Include/W3DDevice/GameClient/Module/W3DScienceModelDraw.h
    Include/W3DDevice/GameClient/Module/W3DSupplyDraw.h
    Include/W3DDevice/GameClient/Module/W3DTankDraw.h
    Include/W3DDevice/GameClient/Module/W3DTankTruckDraw.h
    Include/W3DDevice/GameClient/Module/W3DTracerDraw.h
    Include/W3DDevice/GameClient/Module/W3DTreeDraw.h
    Include/W3DDevice/GameClient/Module/W3DTruckDraw.h
    Include/W3DDevice/GameClient/TerrainTex.h
    Include/W3DDevice/GameClient/TileData.h
#    Include/W3DDevice/GameClient/W3DAssetManager.h
#    Include/W3DDevice/GameClient/W3DAssetManagerExposed.h
#    Include/W3DDevice/GameClient/W3DBibBuffer.h
#    Include/W3DDevice/GameClient/W3DBridgeBuffer.h
#    Include/W3DDevice/GameClient/W3DBufferManager.h
#    Include/W3DDevice/GameClient/W3DCustomScene.h
#    Include/W3DDevice/GameClient/W3DDebugDisplay.h
#    Include/W3DDevice/GameClient/W3DDebugIcons.h
#    Include/W3DDevice/GameClient/W3DDisplay.h
#    Include/W3DDevice/GameClient/W3DDisplayString.h
#    Include/W3DDevice/GameClient/W3DDisplayStringManager.h
#    Include/W3DDevice/GameClient/W3DDynamicLight.h
#    Include/W3DDevice/GameClient/W3DFileSystem.h
    Include/W3DDevice/GameClient/W3DGadget.h
#    Include/W3DDevice/GameClient/W3DGameClient.h
#    Include/W3DDevice/GameClient/W3DGameFont.h
#    Include/W3DDevice/GameClient/W3DGameWindow.h
#    Include/W3DDevice/GameClient/W3DGameWindowManager.h
#    Include/W3DDevice/GameClient/W3DGUICallbacks.h
#    Include/W3DDevice/GameClient/W3DInGameUI.h
#    Include/W3DDevice/GameClient/W3DMirror.h
    Include/W3DDevice/GameClient/W3DMouse.h
    Include/W3DDevice/GameClient/W3DParticleSys.h
#    Include/W3DDevice/GameClient/W3DPoly.h
    Include/W3DDevice/GameClient/W3DProfilerFrameCapture.h
#    Include/W3DDevice/GameClient/W3DProjectedShadow.h
    Include/W3DDevice/GameClient/W3DPropBuffer.h
#    Include/W3DDevice/GameClient/W3DRoadBuffer.h
#    Include/W3DDevice/GameClient/W3DScene.h
    Include/W3DDevice/GameClient/GXRender/Scene/GxL2Check.h
    Include/W3DDevice/GameClient/GXRender/Scene/GxL2Dry.h
    Include/W3DDevice/GameClient/GXRender/Scene/GxScene.h
    Include/W3DDevice/GameClient/W3DShaderManager.h
#    Include/W3DDevice/GameClient/W3DShadow.h
#    Include/W3DDevice/GameClient/W3DShroud.h
    Include/W3DDevice/GameClient/W3DSmudge.h
    Include/W3DDevice/GameClient/W3DSnow.h
#    Include/W3DDevice/GameClient/W3DStatusCircle.h
    Include/W3DDevice/GameClient/W3DTerrainBackground.h
    Include/W3DDevice/GameClient/W3DTerrainTracks.h
    Include/W3DDevice/GameClient/W3DTerrainVisual.h
    Include/W3DDevice/GameClient/W3DTreeBuffer.h
    Include/W3DDevice/GameClient/W3DVideoBuffer.h
    Include/W3DDevice/GameClient/W3DView.h
    Include/W3DDevice/GameClient/W3DWorldSeam.h
    Include/W3DDevice/GameClient/GxWorldPass.h
    Include/W3DDevice/GameClient/W3DScreenshot.h
    Include/W3DDevice/GameClient/GXCap.h
#    Include/W3DDevice/GameClient/W3DVolumetricShadow.h
    Include/W3DDevice/GameClient/W3DWater.h
    Include/W3DDevice/GameClient/W3DWaterTracks.h
#    Include/W3DDevice/GameClient/W3DWaypointBuffer.h
#    Include/W3DDevice/GameClient/W3DWebBrowser.h
    Include/W3DDevice/GameClient/WorldHeightMap.h
#    Include/W3DDevice/GameLogic/W3DGameLogic.h
#    Include/W3DDevice/GameLogic/W3DGhostObject.h
#    Include/W3DDevice/GameLogic/W3DTerrainLogic.h
    Include/Win32Device/Common/Win32BIGFile.h
    Include/Win32Device/Common/Win32BIGFileSystem.h
#    Include/Win32Device/Common/Win32GameEngine.h
#    Include/Win32Device/GameClient/Win32DIKeyboard.h
#    Include/Win32Device/GameClient/Win32DIMouse.h
#    Include/Win32Device/GameClient/Win32Mouse.h
#    Source/MilesAudioDevice/MilesAudioManager.cpp
#    Source/VideoDevice/Bink/BinkVideoPlayer.cpp (Bink SDK Windows-only, using stubs)
#    Source/W3DDevice/Common/System/W3DFunctionLexicon.cpp
    Source/W3DDevice/Common/System/W3DRadar.cpp
#    Source/W3DDevice/Common/Thing/W3DModuleFactory.cpp
#    Source/W3DDevice/Common/Thing/W3DThingFactory.cpp
#    Source/W3DDevice/Common/W3DConvert.cpp
    Source/W3DDevice/GameClient/BaseHeightMap.cpp
    Source/W3DDevice/GameClient/CameraShakeSystem.cpp
    Source/W3DDevice/GameClient/Drawable/Draw/W3DDebrisDraw.cpp
    Source/W3DDevice/GameClient/Drawable/Draw/W3DDefaultDraw.cpp
    Source/W3DDevice/GameClient/Drawable/Draw/W3DDependencyModelDraw.cpp
    Source/W3DDevice/GameClient/Drawable/Draw/W3DLaserDraw.cpp
    Source/W3DDevice/GameClient/Drawable/Draw/W3DModelDraw.cpp
    Source/W3DDevice/GameClient/Drawable/Draw/W3DOverlordAircraftDraw.cpp
    Source/W3DDevice/GameClient/Drawable/Draw/W3DOverlordTankDraw.cpp
    Source/W3DDevice/GameClient/Drawable/Draw/W3DOverlordTruckDraw.cpp
    Source/W3DDevice/GameClient/Drawable/Draw/W3DPoliceCarDraw.cpp
    Source/W3DDevice/GameClient/Drawable/Draw/W3DProjectileStreamDraw.cpp
    Source/W3DDevice/GameClient/Drawable/Draw/W3DPropDraw.cpp
    Source/W3DDevice/GameClient/Drawable/Draw/W3DRopeDraw.cpp
    Source/W3DDevice/GameClient/Drawable/Draw/W3DScienceModelDraw.cpp
    Source/W3DDevice/GameClient/Drawable/Draw/W3DSupplyDraw.cpp
    Source/W3DDevice/GameClient/Drawable/Draw/W3DTankDraw.cpp
    Source/W3DDevice/GameClient/Drawable/Draw/W3DTankTruckDraw.cpp
    Source/W3DDevice/GameClient/Drawable/Draw/W3DTracerDraw.cpp
    Source/W3DDevice/GameClient/Drawable/Draw/W3DTreeDraw.cpp
    Source/W3DDevice/GameClient/Drawable/Draw/W3DTruckDraw.cpp
    Source/W3DDevice/GameClient/FlatHeightMap.cpp
    Source/W3DDevice/GameClient/GUI/Gadget/W3DCheckBox.cpp
    Source/W3DDevice/GameClient/GUI/Gadget/W3DComboBox.cpp
    Source/W3DDevice/GameClient/GUI/Gadget/W3DHorizontalSlider.cpp
    Source/W3DDevice/GameClient/GUI/Gadget/W3DListBox.cpp
    Source/W3DDevice/GameClient/GUI/Gadget/W3DProgressBar.cpp
    Source/W3DDevice/GameClient/GUI/Gadget/W3DPushButton.cpp
    Source/W3DDevice/GameClient/GUI/Gadget/W3DRadioButton.cpp
    Source/W3DDevice/GameClient/GUI/Gadget/W3DStaticText.cpp
    Source/W3DDevice/GameClient/GUI/Gadget/W3DTabControl.cpp
    Source/W3DDevice/GameClient/GUI/Gadget/W3DTextEntry.cpp
    Source/W3DDevice/GameClient/GUI/Gadget/W3DVerticalSlider.cpp
    Source/W3DDevice/GameClient/GUI/GUICallbacks/W3DControlBar.cpp
#    Source/W3DDevice/GameClient/GUI/GUICallbacks/W3DMainMenu.cpp
#    Source/W3DDevice/GameClient/GUI/GUICallbacks/W3DMOTD.cpp
#    Source/W3DDevice/GameClient/GUI/W3DGameFont.cpp
#    Source/W3DDevice/GameClient/GUI/W3DGameWindow.cpp
#    Source/W3DDevice/GameClient/GUI/W3DGameWindowManager.cpp
    Source/W3DDevice/GameClient/HeightMap.cpp
#    Source/W3DDevice/GameClient/Shadow/W3DBufferManager.cpp
#    Source/W3DDevice/GameClient/Shadow/W3DProjectedShadow.cpp
#    Source/W3DDevice/GameClient/Shadow/W3DShadow.cpp
#    Source/W3DDevice/GameClient/Shadow/W3DVolumetricShadow.cpp
    Source/W3DDevice/GameClient/TerrainTex.cpp
    Source/W3DDevice/GameClient/TileData.cpp
#    Source/W3DDevice/GameClient/W3DAssetManager.cpp
#    Source/W3DDevice/GameClient/W3DAssetManagerExposed.cpp
#    Source/W3DDevice/GameClient/W3DBibBuffer.cpp
#    Source/W3DDevice/GameClient/W3DBridgeBuffer.cpp
#    Source/W3DDevice/GameClient/W3DDebugDisplay.cpp
#    Source/W3DDevice/GameClient/W3DDebugIcons.cpp
#    Source/W3DDevice/GameClient/W3DDisplay.cpp
#    Source/W3DDevice/GameClient/W3DDisplayString.cpp
#    Source/W3DDevice/GameClient/W3DDisplayStringManager.cpp
#    Source/W3DDevice/GameClient/W3DDynamicLight.cpp
#    Source/W3DDevice/GameClient/W3DFileSystem.cpp
#    Source/W3DDevice/GameClient/W3DGameClient.cpp
#    Source/W3DDevice/GameClient/W3DInGameUI.cpp
    Source/W3DDevice/GameClient/W3DMouse.cpp
    Source/W3DDevice/GameClient/W3DParticleSys.cpp
#    Source/W3DDevice/GameClient/W3DPoly.cpp
    Source/W3DDevice/GameClient/W3DProfilerFrameCapture.cpp
    Source/W3DDevice/GameClient/W3DPropBuffer.cpp
#    Source/W3DDevice/GameClient/W3DRoadBuffer.cpp
#    Source/W3DDevice/GameClient/W3DScene.cpp
    Source/W3DDevice/GameClient/GXRender/Scene/GxL2Check.cpp
    # GeneralsX @feature Claude 10/10/2026 WS-G Task 5 T3: the device-less wrapper model C6 replays each modelled flush through
    Source/W3DDevice/GameClient/GXRender/Scene/GxL2Model.cpp
    Source/W3DDevice/GameClient/GXRender/Scene/GxL2ModelApply.cpp
    Source/W3DDevice/GameClient/GXRender/Scene/GxL2ModelShader.cpp
    Source/W3DDevice/GameClient/GXRender/Scene/GxL2ModelReplay.cpp
    Source/W3DDevice/GameClient/GXRender/Scene/GxL2Seat.cpp
    Source/W3DDevice/GameClient/GXRender/Scene/GxL2Dry.cpp
    Source/W3DDevice/GameClient/GXRender/Scene/GxScene.cpp
    Source/W3DDevice/GameClient/W3DShaderManager.cpp
    # GeneralsX @feature Claude 10/10/2026 WS-X hybrid spike (render-seam/DESIGN_hybrid.md). Not digested: this file is
    # outside simsourcedigest_watcher.cmake's lists, so listing a render source here leaves the SimID alone.
    Include/W3DDevice/GameClient/GxInterop.h
    Source/W3DDevice/GameClient/GXRender/Interop/GxInterop.cpp
    # GeneralsX @feature E · rw3-K 11/10/2026 GX_RENDERER=native switch (GxNativeInstall.h); not digested
    Include/W3DDevice/GameClient/GxNativeInstall.h
    Source/W3DDevice/GameClient/GXRender/Device/GxNativeInstallOff.cpp
    # GeneralsX @feature Claude 11/10/2026 WS-X2: DrawRun (DESIGN_hybrid §13) - not digested, like GxInterop.
    Include/W3DDevice/GameClient/GxDrawRun.h
    Source/W3DDevice/GameClient/GXRender/Interop/GxDrawRun.cpp
    Include/W3DDevice/GameClient/GxDepthShot.h
    Source/W3DDevice/GameClient/GXRender/Interop/GxDepthShot.cpp
#    Source/W3DDevice/GameClient/W3DShroud.cpp
    Source/W3DDevice/GameClient/W3DSmudge.cpp
    Source/W3DDevice/GameClient/W3DSnow.cpp
#    Source/W3DDevice/GameClient/W3DStatusCircle.cpp
    Source/W3DDevice/GameClient/W3DTerrainBackground.cpp
    Source/W3DDevice/GameClient/W3DTerrainTracks.cpp
    Source/W3DDevice/GameClient/W3DTerrainVisual.cpp
    Source/W3DDevice/GameClient/W3DTreeBuffer.cpp
    Source/W3DDevice/GameClient/W3DVideoBuffer.cpp
    Source/W3DDevice/GameClient/W3DView.cpp
    Source/W3DDevice/GameClient/W3DWorldSeam.cpp
    Source/W3DDevice/GameClient/W3DScreenshot.cpp
    Source/W3DDevice/GameClient/stb_image_write_impl.cpp
    Source/W3DDevice/GameClient/GXCap/GXCapFormat.h
    Source/W3DDevice/GameClient/GXCap/GXCap.cpp
    Source/W3DDevice/GameClient/GXCap/GXCapRecorder.cpp
    Source/W3DDevice/GameClient/GXCap/GXCapRecorder.h
#    Source/W3DDevice/GameClient/W3dWaypointBuffer.cpp
#    Source/W3DDevice/GameClient/W3DWebBrowser.cpp
    Source/W3DDevice/GameClient/Water/W3DWater.cpp
    Source/W3DDevice/GameClient/Water/W3DWaterTracks.cpp
    Source/W3DDevice/GameClient/WorldHeightMap.cpp
#    Source/W3DDevice/GameLogic/W3DGameLogic.cpp
#    Source/W3DDevice/GameLogic/W3DGhostObject.cpp
#    Source/W3DDevice/GameLogic/W3DTerrainLogic.cpp
    Source/Win32Device/Common/Win32BIGFile.cpp
    Source/Win32Device/Common/Win32BIGFileSystem.cpp
#    Source/Win32Device/Common/Win32GameEngine.cpp
#    Source/Win32Device/GameClient/Win32DIMouse.cpp
#    Source/Win32Device/GameClient/Win32Mouse.cpp
)

# GeneralsX @build BenderAI 13/02/2026 Platform-specific file implementations
if(WIN32)
    # Windows-specific files (use Windows API for filesystem)
    list(APPEND GAMEENGINEDEVICE_SRC
        Include/Win32Device/Common/Win32LocalFile.h
        Include/Win32Device/Common/Win32LocalFileSystem.h
        Source/Win32Device/Common/Win32LocalFile.cpp
        Source/Win32Device/Common/Win32LocalFileSystem.cpp
        # GeneralsX @feature Claude 23/09/2026 The MoveFileExA arm of GameClient/GXReplaceFile.h.
        Source/Win32Device/Common/Win32ReplaceFile.cpp
#        Source/Win32Device/Common/Win32OSDisplay.cpp

        # GeneralsX @bugfix Claude 27/07/2026 Build the shared Win32 input files from Core, where
        # they actually live.
        #
        # Win32DIKeyboard.cpp and Win32Mouse.cpp exist ONLY under Core/GameEngineDevice, but both
        # game trees listed them in their own target_sources with per-game-relative paths, so a
        # Windows configure failed with "Cannot find source file". Upstream builds them from Core
        # with the per-game entries commented out; this restores that arrangement.
        #
        # Win32DIMouse.cpp stays out deliberately - it is excluded in this tree and in jmarshall's
        # Win64 port alike; mouse input comes from Win32Mouse.cpp via the window message queue.
        Include/Win32Device/GameClient/Win32DIKeyboard.h
        Include/Win32Device/GameClient/Win32Mouse.h
        Source/Win32Device/GameClient/Win32DIKeyboard.cpp
        Source/Win32Device/GameClient/Win32Mouse.cpp

        # GeneralsX @bugfix Claude 27/07/2026 Build the Miles and Bink devices back on Windows.
        #
        # Both were commented out at the top of this file for the Linux port ("Disable Miles/Bink
        # for Linux (Windows-only)"), but their consumers were never made conditional with them:
        # Win32GameEngine::createAudioManager() still does "new MilesAudioManager" and
        # W3DGameClient::createVideoPlayer() still does "new BinkVideoPlayer". On Windows that is
        # 17 unresolved externals at link (LNK2001/LNK2019). Restoring them here - inside the
        # existing if(WIN32) - is the arrangement upstream has, and leaves the Linux and macOS
        # source list byte for byte as it was; those platforms use the OpenAL manager and the
        # BinkVideoPlayerStub instead.
        #
        # Both compile at x64 now that the root CMakeLists fetches the miles-sdk-stub headers
        # there and Dependencies/Bink supplies bink.h.
        Include/VideoDevice/Bink/BinkVideoPlayer.h
        Source/VideoDevice/Bink/BinkVideoPlayer.cpp
    )

    # GeneralsX @bugfix Claude 06/08/2026 ONE AUDIO BACKEND PER BINARY.
    #
    # Bink stays unconditional above - W3DGameClient::createVideoPlayer() constructs it on Windows
    # regardless of the audio backend, so removing it is the 17-unresolved-externals mistake the
    # comment above documents. Only the Miles AUDIO half is conditional.
    #
    # Why it has to be: with SAGE_USE_OPENAL on, :285-303 below also adds the OpenAL manager, so
    # both backends land in one target. They declare structurally DIFFERENT types under IDENTICAL
    # global names - MAXPROVIDERS, enum PlayingAudioType, enum PlayingWhich, struct PlayingAudio,
    # struct ProviderInfo, struct OpenAudioFile - in
    # Include/MilesAudioDevice/MilesAudioManager.h and
    # Include/OpenALAudioDevice/OpenALAudioManager.h + Source/OpenALAudioDevice/OpenALAudioCache.h.
    # That is a One Definition Rule violation across translation units. MSVC issues NO diagnostic
    # and the linker folds one layout over the other, so it does not fail the build - it produces a
    # binary that reads a Miles PlayingAudio through an OpenAL layout at runtime. Silent corruption
    # is worse than a link error.
    #
    # De-inlining the factory so no single TU sees both headers is necessary but NOT sufficient,
    # because the collision is cross-TU. Compiling only one backend is. The reference win64 port
    # (references/jmarshall-win64-modern) instead renamed every colliding symbol in its OpenAL
    # header - that is the alternative if both backends ever genuinely have to coexist.
    #
    # NOTE for the audio bring-up: Core/Tools/MapCacheBuilder and both WorldBuilder targets
    # construct MilesAudioManager directly and default ON, so an OpenAL build needs
    # RTS_BUILD_CORE_TOOLS, RTS_BUILD_ZEROHOUR_TOOLS and RTS_BUILD_GENERALS_TOOLS OFF until those
    # three call sites are made backend-agnostic. The win64 preset sets all three, with
    # SAGE_USE_OPENAL (25/09/2026); before that they lived only in one machine's CMakeCache.txt.
    if(NOT SAGE_USE_OPENAL)
        list(APPEND GAMEENGINEDEVICE_SRC
            Include/MilesAudioDevice/MilesAudioManager.h
            Source/MilesAudioDevice/MilesAudioManager.cpp
        )
    endif()
endif()

# Add C++ 17 FileSystem implementation for non-VS6 builds
if(NOT IS_VS6_BUILD)
    list(APPEND GAMEENGINEDEVICE_SRC
        Include/StdDevice/Common/StdBIGFile.h
        Include/StdDevice/Common/StdBIGFileSystem.h
        Include/StdDevice/Common/StdLocalFile.h
        Include/StdDevice/Common/StdLocalFileSystem.h
        Source/StdDevice/Common/StdBIGFile.cpp
        Source/StdDevice/Common/StdBIGFileSystem.cpp
        Source/StdDevice/Common/StdLocalFile.cpp
        Source/StdDevice/Common/StdLocalFileSystem.cpp
    )
endif()

add_library(corei_gameenginedevice_private INTERFACE)
# GeneralsX @feature Claude 10/10/2026 WS-X: GxInterop needs Vulkan TYPES only (VK_NO_PROTOTYPES; every entry point comes from
# DXVK's own vkGetInstanceProcAddr). DXVK's Vulkan-Headers are appended LAST to the include path, so a Vulkan SDK already on it
# (macOS) still wins for every other translation unit, and Linux/Windows trees without one get DXVK's. vulkan_core.h includes
# vk_video/... relative to that root, so the root itself must be on the path (a macro include of the file alone fails on Linux).
set(GX_VULKAN_HEADERS "${CMAKE_SOURCE_DIR}/references/fbraz3-dxvk/include/vulkan/include")
if(EXISTS "${GX_VULKAN_HEADERS}/vulkan/vulkan_core.h")
    target_include_directories(corei_gameenginedevice_private INTERFACE "${GX_VULKAN_HEADERS}")
    target_compile_definitions(corei_gameenginedevice_private INTERFACE GX_HAVE_VULKAN_HEADERS=1)
else()
    message(STATUS "GxInterop: no ${GX_VULKAN_HEADERS}/vulkan/vulkan_core.h - the hybrid spike builds as 'unavailable'")
endif()
add_library(corei_gameenginedevice_public INTERFACE)

target_sources(corei_gameenginedevice_private INTERFACE ${GAMEENGINEDEVICE_SRC})

target_include_directories(corei_gameenginedevice_public INTERFACE
    Include
)

target_link_libraries(corei_gameenginedevice_private INTERFACE
    corei_always
    corei_main
    stb
)

# GeneralsX @build BenderAI 13/02/2026 Platform-specific multimedia stubs
target_link_libraries(corei_gameenginedevice_public INTERFACE
    corei_gameengine_public
)

if(WIN32)
    target_link_libraries(corei_gameenginedevice_public INTERFACE
        binkloader
        d3d8lib
        milesloader
    )
endif()

# GeneralsX @build BenderAI 13/02/2026 OpenAL audio library for Linux
# GeneralsX @bugfix fbraz 10/03/2026 Use FetchContent target when available, fallback to find_package
# GeneralsX @refactor fbraz3 31/03/2026 OpenALAudioDevice moved from GeneralsMD to Core (shared by Generals and Zero Hour)
if(SAGE_USE_OPENAL)
    if(TARGET OpenAL::OpenAL)
        # FetchContent-provided openal-soft: interface includes propagated via target
        target_link_libraries(corei_gameenginedevice_public INTERFACE OpenAL::OpenAL)
    else()
        # System install of OpenAL (Linux with libopenal-dev, etc.)
        find_package(OpenAL REQUIRED)
        target_include_directories(corei_gameenginedevice_public INTERFACE ${OPENAL_INCLUDE_DIR})
        target_link_libraries(corei_gameenginedevice_public INTERFACE ${OPENAL_LIBRARY})
    endif()

    target_sources(corei_gameenginedevice_private INTERFACE
        Include/OpenALAudioDevice/AudioDecoder.h
        Include/OpenALAudioDevice/OpenALAudioManager.h
        Include/OpenALAudioDevice/OpenALAudioStream.h
        Source/OpenALAudioDevice/AudioDecoder.cpp
        Source/OpenALAudioDevice/OpenALAudioManager.cpp
        Source/OpenALAudioDevice/OpenALAudioStream.cpp
        Source/OpenALAudioDevice/OpenALAudioCache.cpp
    )
    # OpenALAudioCache.h is a private header co-located with its source
    target_include_directories(corei_gameenginedevice_private INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/Source/OpenALAudioDevice
    )

    # GeneralsX @feature Claude 06/08/2026 ONE AUDIO DECODER, THE SAME ONE, ON ALL FOUR PLATFORMS.
    #
    # dr_wav + dr_mp3 everywhere. Not "FFmpeg where it exists, dr where it does not" - that was the
    # first cut of this and it was wrong, for a reason worth stating because it is easy to re-derive
    # badly.
    #
    # getFileLengthMS is SIMULATION STATE. ScriptEngine::isSpeechComplete / isAudioComplete turn it
    # into `getFrame() + audioLength/MSEC_PER_LOGICFRAME` and latch that, so two peers that disagree
    # about a file's length take different script branches. Both backends derive the duration the
    # same way, from the decoded sample count - so the decoder choice IS a simulation input.
    #
    # Measured 06/08/2026 by decoding all 3530 audio entries in the 25 shipped BIGs twice, once each
    # way, and comparing decoded sample counts:
    #
    #   3523 / 3530 agree EXACTLY   - every WAV, both PCM (0x0001) and IMA-ADPCM (0x0011)
    #   7 / 3530 differ             - exactly the 7 MP3s, every one by exactly 3456 samples
    #
    # 3456 is 3 x 1152: three MPEG granules of decoder delay that dr_mp3 trims and FFmpeg emits.
    # At 60 Hz that is 4-5 logic frames of disagreement on Data\Audio\Tracks\*.mp3. Using the same
    # decoder on every platform removes that by construction rather than by luck.
    #
    # BY LUCK is not hyperbole - it is what the tree was already relying on elsewhere.
    # MilesAudioManager::hasMusicTrackCompleted (MilesAudioManager.cpp:1337) is a LIVE
    # implementation returning TRUE off AIL_stream_loop_count. It agreed with the other platforms
    # only because the 13 KB x64 Miles stub never opens a device, so findActiveMusic walks a
    # permanently empty m_playingStreams. Accidental agreement, one stub away from divergence.
    #
    # THIS CHOICE IS NOT IN THE sourceID DIGEST. Core/GameEngineDevice/ is outside the watched set,
    # so two peers built with different decoders would present identical sourceIDs and still
    # simulate differently. That is exactly why there is no cache option here to get wrong: it is
    # DR, unconditionally. The durable fix is to stop deriving duration from the decoder at all -
    # parse it from the container header identically everywhere - which makes the decoder choice
    # simulation-irrelevant. Until that lands, do not add a knob.
    #
    # Coverage is complete, so nothing regresses to a silent fallback: 2572 IMA-ADPCM, 951 PCM,
    # 7 MP3, zero unhandled. This also keeps FFmpeg - and therefore vcpkg.json, which IS digested -
    # out of the Windows build entirely. FFmpeg remains for VIDEO only.
    #
    # Note this replaced an `if(NOT RTS_BUILD_OPTION_FFMPEG) -> add FFmpegFile.cpp` block that
    # could never have worked: it compiled libav-calling code without ever adding the libav
    # include directories or link libraries.
    target_compile_definitions(corei_gameenginedevice_public INTERFACE SAGE_AUDIO_DECODER_DR)
endif()

if(RTS_BUILD_OPTION_FFMPEG)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavcodec libavformat libavutil libswscale)

    if(FFMPEG_FOUND)
        target_sources(corei_gameenginedevice_private INTERFACE
            Include/VideoDevice/FFmpeg/FFmpegFile.h
            Include/VideoDevice/FFmpeg/FFmpegVideoPlayer.h
            Source/VideoDevice/FFmpeg/FFmpegFile.cpp
            Source/VideoDevice/FFmpeg/FFmpegVideoPlayer.cpp
        )
        target_include_directories(corei_gameenginedevice_private INTERFACE ${FFMPEG_INCLUDE_DIRS})

        include(${CMAKE_SOURCE_DIR}/cmake/ffmpeg_framework_fix.cmake)
        sage_fix_ffmpeg_framework_flags()

        target_link_libraries(corei_gameenginedevice_private INTERFACE PkgConfig::FFMPEG)
        target_compile_definitions(corei_gameenginedevice_public INTERFACE RTS_HAS_FFMPEG)
    endif()
endif()

# GeneralsX @feature E · rw1-B 10/10/2026 native renderer device layer (PLAN_native-renderer WS-B): gx_vkctx + its
# self-test. Render-only, outside the digested paths; OFF until the session switch (WS-J) wires a backend to it. It also
# builds standalone in minutes: cmake -S Core/GameEngineDevice/Source/W3DDevice/GameClient/GXRender/Vk -B <dir>.
# GeneralsX @feature E · rw3-K 11/10/2026 L1 in the game (render-seam/DESIGN_l1-device.md §12): GX_RENDER_NATIVE=ON links
# GXRender/{Vk,Res,Pipeline,Record,Device} into the game and compiles the GX_RENDERER=native session switch. OFF (default)
# = today's binary. Not digested (outside simsourcedigest_watcher.cmake).
# GeneralsX @feature D · render-line-0967 21/10/2026 (lead C44; wh1ter0se 21/10: native ships OPT-IN in 0.9.67): the DEFAULT is ON for macOS
# and Windows x64 MSVC - so the shipping presets macos-vulkan and win64 compile the native renderer in - and OFF for iOS and Linux
# (ios-vulkan, linux64-deploy). Here and not in CMakePresets.json, which the SimID digest takes whole: no digested file reads
# GX_RENDER_NATIVE, and simid-closure on a native build is the checked proof of that boundary. Compiled in is not switched on: the
# session is classic unless Options.ini GXRenderer = native (GameClient/GXRendererKey.h). -DGX_RENDER_NATIVE=OFF still builds without.
# The option() below says ON - what macOS and Windows x64 get (and what scripts/tooling/gx-knobs.py reads: a literal default).
# Every other platform starts from OFF: its cache entry is seeded before option() runs, which then leaves it alone. An explicit
# -DGX_RENDER_NATIVE=ON still builds an iOS native test arm (native-ipad), as before.
if((APPLE AND NOT CMAKE_SYSTEM_NAME STREQUAL "iOS") OR (WIN32 AND MSVC AND CMAKE_SIZEOF_VOID_P EQUAL 8))
    set(_gx_render_native_ships ON)
else()
    set(_gx_render_native_ships OFF)
endif()
if(NOT _gx_render_native_ships AND NOT DEFINED CACHE{GX_RENDER_NATIVE})
    set(GX_RENDER_NATIVE OFF CACHE BOOL "Build the native d3d8 device (GXRender/Device) into the game (default ON on macOS + Windows x64 MSVC)")
endif()
# A build tree configured before this change holds GX_RENDER_NATIVE=OFF in its cache from the old OFF default, and option() never
# overrides a cache entry - the verify and release trees would stay classic-only without a word. An entry still carrying the OLD
# option()'s help text was made by that default, so it takes the new one, ONCE (the entry then has the new text). An explicit
# -DGX_RENDER_NATIVE=OFF on the configure is kept (measured, CMake 4: the -D replaces the text). Pass it again to opt out.
if(_gx_render_native_ships AND DEFINED CACHE{GX_RENDER_NATIVE} AND NOT GX_RENDER_NATIVE)
    get_property(_gx_render_native_help CACHE GX_RENDER_NATIVE PROPERTY HELPSTRING)
    if(_gx_render_native_help STREQUAL "Build the native d3d8 device (GXRender/Device) into the game")
        message(STATUS "GX_RENDER_NATIVE: this tree's OFF came from the old default - now ON (-DGX_RENDER_NATIVE=OFF to opt out)")
        set(GX_RENDER_NATIVE ON CACHE BOOL "Build the native d3d8 device (GXRender/Device) into the game (default ON on macOS + Windows x64 MSVC)" FORCE)
    endif()
endif()
option(GX_RENDER_NATIVE "Build the native d3d8 device (GXRender/Device) into the game (default ON on macOS + Windows x64 MSVC)" ON)
# GeneralsX @feature D · native-windows-1 13/10/2026 Windows x64 MSVC qualifies too (DESIGN §2 row S2): there gx_device takes the
# game's own DirectX 8 headers (d3d8lib = min-dx8-sdk + the Windows SDK), never DXVK's <windows.h> shim (Codex rw3K-code-r2), and
# the host is the game's Win32 window (GxNativeHostWin32.cpp). Linux, iOS and 32-bit Windows keep the OFF stub.
if(GX_RENDER_NATIVE AND NOT (APPLE OR (WIN32 AND MSVC AND CMAKE_SIZEOF_VOID_P EQUAL 8)))
    message(WARNING "GX_RENDER_NATIVE is macOS and Windows x64 (MSVC) only: forcing it OFF")
    set(GX_RENDER_NATIVE OFF CACHE BOOL "" FORCE)
endif()
if(GX_RENDER_NATIVE)
    set(GX_RENDER_VK ON CACHE BOOL "" FORCE)
    # The GXRender libraries need C++17 (std::filesystem, shared_mutex); the game is compiled as C++14, and these
    # directories inherit the variable. Directory-scoped: restored after the native block at the end of this file.
    set(GX_SAVED_CXX_STANDARD "${CMAKE_CXX_STANDARD}")
    set(CMAKE_CXX_STANDARD 17)
endif()
option(GX_RENDER_VK "Build the native Vulkan renderer libraries (GXRender/Vk)" OFF)
if(GX_RENDER_VK)
    add_subdirectory(Source/W3DDevice/GameClient/GXRender/Vk)
endif()

if(GX_RENDER_NATIVE)
    # gx_device pulls Record -> Pipeline -> Res -> Vk itself; the d3d8 headers are the game's own (d3d8lib), so the
    # standalone default include is only a fallback.
    set(GX_DEVICE_TESTS OFF CACHE BOOL "" FORCE)
    add_subdirectory(Source/W3DDevice/GameClient/GXRender/Device)
    if(WIN32)
        set(_gx_native_host GxNativeHostWin32.cpp)
    else()
        set(_gx_native_host GxNativeHostSDL.cpp)
    endif()
    target_sources(corei_gameenginedevice_private INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/Source/W3DDevice/GameClient/GXRender/Device/GxNativeInstall.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Source/W3DDevice/GameClient/GXRender/Device/${_gx_native_host}
        # D · render-motion-units-1 (A35): the object-velocity bridge, game side; native only (it includes GXRender/Pipeline headers)
        ${CMAKE_CURRENT_SOURCE_DIR}/Source/W3DDevice/GameClient/GXRender/Scene/GxMotionUnits.cpp)
    # In the game, <d3d8.h> resolves through the game's CompatLib windows.h -> windows_compat.h, which includes WWLib's bittype.h.
    foreach(_gxt gx_device gx_record gx_pipeline gx_res gx_vkctx)
        if(TARGET ${_gxt})
            target_include_directories(${_gxt} PRIVATE "${CMAKE_SOURCE_DIR}/Core/Libraries/Source/WWVegas/WWLib")
        endif()
    endforeach()
    # GeneralsX @performance D · native-windows-1 13/10/2026 MSVC's RelWithDebInfo (the shipped Windows config) is /O2 /Ob1: only
    # functions marked inline are inlined, while clang -O2 on the Mac inlines freely. The GXRender libraries alone get /Ob2 (the last /Ob
    # wins); game sources compile exactly as before. Measured (PREREG_T A38 am.1, wh1ter0se's PC, ABAB n=2+2): native per-draw main-thread
    # CPU -5.9..-8.1% in every cross-pair; an n=3+3 confirmation is owed before any "faster" claim.
    if(MSVC)
        foreach(_gxt gx_device gx_record gx_pipeline gx_res gx_vkctx)
            if(TARGET ${_gxt})
                target_compile_options(${_gxt} PRIVATE $<$<CONFIG:RelWithDebInfo>:/Ob2>)
            endif()
        endforeach()
    endif()
    # GxNativeUuid.h keys its IUnknown helper on this; the define on corei_gameenginedevice_public does not reach the library.
    target_compile_definitions(gx_device PRIVATE GX_RENDER_NATIVE=1)
    target_link_libraries(corei_gameenginedevice_public INTERFACE gx_device)
    target_compile_definitions(corei_gameenginedevice_public INTERFACE GX_RENDER_NATIVE=1)
    # GeneralsX @feature B · dlss-streamline-1 13/10/2026 GX_STREAMLINE (default OFF; PREREG A36; DLSS_INTEGRATION.md rule 5a: test
    # builds only until the lead + wh1ter0se document a licence basis): NVIDIA Streamline (DLSS SR) behind the seam, Windows x64. Headers
    # from GX_STREAMLINE_SDK at build time; NVIDIA's DLLs are never in the tree or the zip. The define is gx_device's alone: no game
    # source compiles differently.
    option(GX_STREAMLINE "NVIDIA Streamline / DLSS in the native renderer (Windows x64 test builds only)" OFF)
    if(GX_STREAMLINE AND WIN32)
        set(GX_STREAMLINE_TESTS OFF CACHE BOOL "" FORCE)
        add_subdirectory(Source/W3DDevice/GameClient/GXRender/Upscale)
        target_link_libraries(gx_device PRIVATE gx_streamline)
        target_compile_definitions(gx_device PRIVATE GX_STREAMLINE=1)
        # B · mac-upscaler-u2 18/10/2026 (dlss-integrator ruling 18/10 13:0xZ): GXGraphicsSettings.cpp resolves GXUpscaler=dlss to off in a
        # build without Streamline, so it needs to know this build has it. A SOURCE property on that one file only, set once the game's
        # engine targets exist (they are made after Core is processed); no other game source compiles differently.
        function(gx_u2_streamline_define_for_settings)
            foreach(_t z_gameengine g_gameengine)
                if(TARGET ${_t})
                    set_source_files_properties("${CMAKE_SOURCE_DIR}/Core/GameEngine/Source/GameClient/GXGraphicsSettings.cpp"
                        TARGET_DIRECTORY ${_t} PROPERTIES COMPILE_DEFINITIONS GX_STREAMLINE=1)
                endif()
            endforeach()
        endfunction()
        cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL gx_u2_streamline_define_for_settings)
    endif()
    set(CMAKE_CXX_STANDARD "${GX_SAVED_CXX_STANDARD}")
endif()
