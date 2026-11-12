# DirectX 8 headers and rendering backend selection
# GeneralsX: Session 18
# Fighter19's approach: Fetch ONE OR THE OTHER, never both
#
# On Windows: Use min-dx8-sdk (minimal Windows DirectX headers + libs)
# On Linux:   Use DXVK native pre-built tarball (DirectX→Vulkan translation)
# On macOS:   Build DXVK from source using Meson + MoltenVK (DirectX→Metal bridge)
#
# CRITICAL: Mixing headers causes conflicts - dx8-src has incomplete types,
# DXVK has full DirectX8+Wine headers. Compiler picks first path = wrong headers.
#
# macOS DXVK build (Session 61, 24/02/2026):
#   DXVK 2.6 builds natively on macOS arm64 via its "native" build mode.
#   macOS fixes are maintained in the DXVK fork history consumed by this build.
#   This project no longer applies local patch scripts during configure/build.
#
# Reference: (historical patch rationale)

set(DXVK_VERSION "v2.6")

# Give SAGE_USE_DX8 a default on Windows. Left undefined, Windows would fall through to the Linux arm below and unpack
# DXVK's Linux tarball, whose stand-in windows.h and windows_base.h shadow the real Windows SDK (C2371 redefinitions).
# On Windows DXVK is a drop-in d3d8.dll with no headers and dx8wrapper.cpp loads D3D8.DLL at runtime, so only the
# min-dx8-sdk headers are needed to compile.
if(WIN32 AND NOT DEFINED SAGE_USE_DX8)
  set(SAGE_USE_DX8 ON CACHE BOOL "Use the native DirectX 8 SDK headers/libs (Windows only)")
endif()

if(SAGE_USE_DX8)
  # Windows: Fetch min-dx8-sdk for native DirectX 8
  FetchContent_Declare(
    dx8
    GIT_REPOSITORY https://github.com/TheSuperHackers/min-dx8-sdk.git
    GIT_TAG        7bddff8c01f5fb931c3cb73d4aa8e66d303d97bc
  )
  FetchContent_MakeAvailable(dx8)

  # min-dx8-sdk carries its own CMakeLists that creates the d3d8lib INTERFACE target and points its
  # include/link dirs at the SDK root. Nothing else here needs to configure d3d8lib on Windows.

  # GeneralsX: Pin d3dx8.lib by absolute path under MSVC.
  #
  # min-dx8-sdk requests it as the bare name "d3dx8". GeneralsMD/Code/CompatLib/CMakeLists.txt
  # creates an EMPTY INTERFACE target literally named d3dx8 on 32-bit Windows - its real d3dx8
  # sources are gated on Win64/macOS/Linux - and a CMake target always wins over a library file
  # name. The bare reference therefore resolves to that empty target and d3dx8.lib drops off the
  # link line entirely, leaving D3DXCreateTexture / D3DXCreateTextureFromFileExA / D3DXFilterTexture
  # / D3DXLoadSurfaceFromSurface / D3DXGetFVFVertexSize unresolved. An absolute path can never be
  # mistaken for a target name. Remove this once CompatLib stops creating the stub on Windows.
  #
  # GeneralsX: 32-bit only - see the x64 block below for why.
  if(MSVC AND CMAKE_SIZEOF_VOID_P EQUAL 4 AND EXISTS "${dx8_SOURCE_DIR}/d3dx8.lib")
    target_link_libraries(d3d8lib INTERFACE "${dx8_SOURCE_DIR}/d3dx8.lib")
  endif()

  # Keep min-dx8-sdk's x86 import libraries off the x64 link: d3d8.lib, d3dx8.lib, dinput8.lib and dxguid.lib are all
  # x86 (LNK1112 on x64), and its link dir is added with BEFORE so it would win over the Windows SDK's x64 libs.
  # dinput8/dxguid come from the Windows SDK; d3dx8 is the static target built in CompatLib; d3d8 is loaded at runtime
  # by dx8wrapper.cpp, so nothing links d3d8.lib. Headers are untouched.
  if(MSVC AND CMAKE_SIZEOF_VOID_P EQUAL 8)
    get_target_property(_dx8_link_dirs d3d8lib INTERFACE_LINK_DIRECTORIES)
    if(_dx8_link_dirs)
      list(REMOVE_ITEM _dx8_link_dirs "${dx8_SOURCE_DIR}")
      set_target_properties(d3d8lib PROPERTIES INTERFACE_LINK_DIRECTORIES "${_dx8_link_dirs}")
    endif()
    get_target_property(_dx8_link_libs d3d8lib INTERFACE_LINK_LIBRARIES)
    if(_dx8_link_libs)
      list(REMOVE_ITEM _dx8_link_libs "d3d8")
      set_target_properties(d3d8lib PROPERTIES INTERFACE_LINK_LIBRARIES "${_dx8_link_libs}")
    endif()
  endif()

  message(STATUS "Using DirectX 8 SDK (Windows native): ${dx8_SOURCE_DIR}")

elseif(APPLE AND SAGE_USE_MOLTENVK)
  # macOS: Build DXVK 2.6 from source using Meson + MoltenVK
  # GeneralsX: Phase 5 macOS port (Session 61)
  find_program(MESON_EXECUTABLE meson HINTS /usr/local/bin /opt/homebrew/bin)
  find_program(NINJA_EXECUTABLE ninja HINTS /usr/local/bin /opt/homebrew/bin)

  if(NOT MESON_EXECUTABLE)
    message(FATAL_ERROR "DXVK macOS build requires meson: brew install meson")
  endif()
  if(NOT NINJA_EXECUTABLE)
    message(FATAL_ERROR "DXVK macOS build requires ninja: brew install ninja")
  endif()

  # Detect host architecture so Clang targets the correct slice.
  # IMPORTANT: prefer CMAKE_OSX_ARCHITECTURES (set by the preset) over uname -m.
  # On Apple Silicon Macs running CMake / meson via Rosetta, uname -m returns
  # x86_64 even though the native executable arch is arm64. Using CMAKE_OSX_ARCHITECTURES
  # (e.g. "arm64" from the macos-vulkan preset) avoids building an x86_64 dylib that
  # the arm64 game binary cannot dlopen.
  if(CMAKE_OSX_ARCHITECTURES)
    # Use the first entry (handles "arm64;x86_64" fat-binary requests too)
    list(GET CMAKE_OSX_ARCHITECTURES 0 DXVK_HOST_ARCH)
  else()
    execute_process(
      COMMAND uname -m
      OUTPUT_VARIABLE DXVK_HOST_ARCH
      OUTPUT_STRIP_TRAILING_WHITESPACE
    )
  endif()
  message(STATUS "Building DXVK ${DXVK_VERSION} for macOS/${DXVK_HOST_ARCH} with Meson (${MESON_EXECUTABLE})")

  include(ExternalProject)
  # GeneralsX: Add explicit source mode to keep remote branch updates deterministic by default.
  set(DXVK_LOCAL_FORK_DIR "${CMAKE_SOURCE_DIR}/references/fbraz3-dxvk")
  option(SAGE_DXVK_USE_LOCAL_FORK "Build DXVK from local references/fbraz3-dxvk checkout" OFF)

  if(SAGE_DXVK_USE_LOCAL_FORK AND EXISTS "${DXVK_LOCAL_FORK_DIR}/.git")
    set(DXVK_SOURCE_DIR "${DXVK_LOCAL_FORK_DIR}")
    message(STATUS "DXVK macOS build: using local fork source at ${DXVK_SOURCE_DIR}")
    # Patches/dxvk-ios.patch carries the SDL3 drawable fixes and a bundle-relative MoltenVK dlopen
    # list - apply it idempotently, and fail the configure otherwise so an unpatched DXVK never ships.
    # It is needed on every Retina Apple display: unpatched, getWindowSize returns points while the swapchain takes
    # pixels, so at density 2.0 the picture lands in a corner and the mouse rect is off by the same factor.
    execute_process(
      COMMAND git -C "${DXVK_LOCAL_FORK_DIR}" apply --reverse --check "${CMAKE_SOURCE_DIR}/Patches/dxvk-ios.patch"
      RESULT_VARIABLE DXVK_PATCH_ALREADY_APPLIED
      ERROR_QUIET)
    if(NOT DXVK_PATCH_ALREADY_APPLIED EQUAL 0)
      execute_process(
        COMMAND git -C "${DXVK_LOCAL_FORK_DIR}" apply "${CMAKE_SOURCE_DIR}/Patches/dxvk-ios.patch"
        RESULT_VARIABLE DXVK_PATCH_RESULT)
      if(NOT DXVK_PATCH_RESULT EQUAL 0)
        message(FATAL_ERROR "Failed to apply Patches/dxvk-ios.patch to references/fbraz3-dxvk — the Apple DXVK build requires it.")
      endif()
      message(STATUS "DXVK Apple: applied Patches/dxvk-ios.patch")
    else()
      message(STATUS "DXVK Apple: Patches/dxvk-ios.patch already applied")
    endif()
  elseif(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    # The remote clone has no way to receive the iOS patch; a silent fallback
    # here would produce dylibs that die at Vulkan init on device.
    message(FATAL_ERROR "iOS DXVK requires the local fork submodule. Run: git submodule update --init references/fbraz3-dxvk")
  else()
    set(DXVK_SOURCE_DIR "${CMAKE_BINARY_DIR}/_deps/dxvk-src-fbraz3")
    message(STATUS "DXVK macOS build: using GitHub source clone at ${DXVK_SOURCE_DIR}")
  endif()
  set(DXVK_BUILD_DIR  "${CMAKE_BINARY_DIR}/_deps/dxvk-build-macos")
  set(DXVK_D3D8_LIB  "${DXVK_BUILD_DIR}/src/d3d8/libdxvk_d3d8.0.dylib")
  set(DXVK_D3D9_LIB  "${DXVK_BUILD_DIR}/src/d3d9/libdxvk_d3d9.0.dylib")

  # Detect Vulkan SDK location for Meson configuration.
  # VULKAN_SDK must point to the platform subdir (e.g. ~/VulkanSDK/1.4.x/macOS)
  # where lib/libvulkan.dylib and lib/libMoltenVK.dylib live.
  # GeneralsX: Normalize env path to macOS platform subdir
  set(VULKAN_SDK_ENV "$ENV{VULKAN_SDK}")

  # If VULKAN_SDK points to the version root (has macOS/ subdir), normalize it
  if(VULKAN_SDK_ENV AND EXISTS "${VULKAN_SDK_ENV}/macOS/lib/libMoltenVK.dylib")
    set(VULKAN_SDK_ENV "${VULKAN_SDK_ENV}/macOS")
    message(STATUS "DXVK macOS build: Normalized VULKAN_SDK to platform subdir: ${VULKAN_SDK_ENV}")
  endif()

  if(NOT VULKAN_SDK_ENV OR NOT EXISTS "${VULKAN_SDK_ENV}/lib/libMoltenVK.dylib")
    # Try home directory: look for ~/VulkanSDK/*/macOS
    file(GLOB VULKAN_HOME_DIRS "$ENV{HOME}/VulkanSDK/*/macOS")
    if(VULKAN_HOME_DIRS)
      list(SORT VULKAN_HOME_DIRS)
      list(REVERSE VULKAN_HOME_DIRS)
      list(GET VULKAN_HOME_DIRS 0 POTENTIAL_SDK)
      if(EXISTS "${POTENTIAL_SDK}/lib/libMoltenVK.dylib")
        set(VULKAN_SDK_ENV "${POTENTIAL_SDK}")
      endif()
    endif()
  endif()

  if(NOT VULKAN_SDK_ENV OR NOT EXISTS "${VULKAN_SDK_ENV}/lib/libMoltenVK.dylib")
    # Try common Homebrew locations
    foreach(BREW_PATH "/usr/local/Caskroom/vulkan-sdk/latest/VulkanSDK/macOS" "/opt/homebrew/Caskroom/vulkan-sdk/latest/VulkanSDK/macOS")
      if(EXISTS "${BREW_PATH}/lib/libMoltenVK.dylib")
        set(VULKAN_SDK_ENV "${BREW_PATH}")
        break()
      endif()
    endforeach()
  endif()

  if(VULKAN_SDK_ENV AND EXISTS "${VULKAN_SDK_ENV}/lib/libMoltenVK.dylib")
    message(STATUS "DXVK macOS build: Using Vulkan SDK at ${VULKAN_SDK_ENV}")
    # GeneralsX: Put the SDK's bin on PATH for the meson sub-build too.
    #
    # DXVK's meson.build requires glslangValidator to compile its shaders. The Vulkan SDK ships it
    # in ${VULKAN_SDK}/bin, but that directory is not normally on PATH, so meson fails with
    # "Program 'glslang glslangValidator' not found or not executable" - on a machine where the
    # tool is present the whole time. Setting VULKAN_SDK alone is not enough; meson looks the
    # program up on PATH.
    #
    # This has cost time twice, and it re-bites every fresh shell because exporting PATH by hand
    # does not persist. Passing it here makes the failure impossible rather than documented.
    set(VULKAN_SDK_ENV_VAR "VULKAN_SDK=${VULKAN_SDK_ENV}" "PATH=${VULKAN_SDK_ENV}/bin:$ENV{PATH}")
  else()
    message(WARNING "DXVK macOS build: Vulkan SDK / MoltenVK not found; Meson will search system paths")
    if(VULKAN_SDK_ENV)
      message(STATUS "  VULKAN_SDK checked: ${VULKAN_SDK_ENV}")
    endif()
    set(VULKAN_SDK_ENV_VAR "")
  endif()

  # iOS cross-compiles DXVK with a meson cross file (iPhoneOS sysroot); macOS uses
  # the native file. Arch/sysroot flags come from the cross file on iOS and from -D on macOS.
  if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    # The cross file is generated from a template so the iPhoneOS SDK path comes
    # from xcrun (Xcode-beta / renamed installs) instead of a hardcoded Xcode.app.
    execute_process(COMMAND xcrun --sdk iphoneos --show-sdk-path
                    OUTPUT_VARIABLE IOS_SDK OUTPUT_STRIP_TRAILING_WHITESPACE
                    COMMAND_ERROR_IS_FATAL ANY)
    configure_file(${CMAKE_SOURCE_DIR}/cmake/meson-arm64-ios-cross.ini.in
                   ${CMAKE_BINARY_DIR}/meson-arm64-ios-cross.ini @ONLY)
    set(DXVK_MESON_MACHINE_ARGS --cross-file ${CMAKE_BINARY_DIR}/meson-arm64-ios-cross.ini)
  else()
    # The SDK and deployment target go on meson's command line (-isysroot), else the sub-build takes the compiler's
    # default SDK, which may be newer than the Xcode linker can read.
    # Not in the native file: meson keeps the machine file a build dir was first set up with; -D is re-applied on every
    # --reconfigure. It replaces the native file's c_args, hence the -arch. A SYSROOT given as a name is resolved via xcrun.
    set(DXVK_MACOS_SDK "${CMAKE_OSX_SYSROOT}")
    if(NOT IS_DIRECTORY "${DXVK_MACOS_SDK}")
      execute_process(COMMAND xcrun --sdk macosx --show-sdk-path
                      OUTPUT_VARIABLE DXVK_MACOS_SDK OUTPUT_STRIP_TRAILING_WHITESPACE
                      COMMAND_ERROR_IS_FATAL ANY)
    endif()
    set(DXVK_MACOS_ARGS "'-arch','arm64','-isysroot','${DXVK_MACOS_SDK}'")
    if(CMAKE_OSX_DEPLOYMENT_TARGET)
      string(APPEND DXVK_MACOS_ARGS ",'-mmacosx-version-min=${CMAKE_OSX_DEPLOYMENT_TARGET}'")
    endif()
    set(DXVK_MESON_MACHINE_ARGS --native-file ${CMAKE_SOURCE_DIR}/cmake/meson-arm64-native.ini
        "-Dc_args=[${DXVK_MACOS_ARGS}]" "-Dcpp_args=[${DXVK_MACOS_ARGS}]"
        "-Dc_link_args=[${DXVK_MACOS_ARGS}]" "-Dcpp_link_args=[${DXVK_MACOS_ARGS}]")
  endif()

  # Generate a pkg-config file for the in-tree (FetchContent) SDL3 so meson's
  # dependency('SDL3') resolves to it. Without this, meson silently falls back to a
  # system SDL2 (e.g. Homebrew) and compiles the WSI as Sdl2WsiDriver, which cannot
  # drive the SDL3 window the game creates (D3D device creation then fails at runtime).
  set(DXVK_SDL3_PC_DIR "${CMAKE_BINARY_DIR}/sdl3-pkgconfig")
  file(WRITE "${DXVK_SDL3_PC_DIR}/sdl3.pc"
"prefix=${CMAKE_BINARY_DIR}/_deps
libdir=\${prefix}/sdl3-build
includedir=\${prefix}/sdl3-src/include

Name: sdl3
Description: Simple DirectMedia Layer (in-tree FetchContent build)
Version: 3.4.2
Libs: -L\${libdir} -lSDL3
Cflags: -I\${includedir}
")
  if(DEFINED ENV{PKG_CONFIG_PATH})
    set(DXVK_PKG_CONFIG_PATH "${DXVK_SDL3_PC_DIR}:$ENV{PKG_CONFIG_PATH}")
  else()
    set(DXVK_PKG_CONFIG_PATH "${DXVK_SDL3_PC_DIR}")
  endif()
  set(DXVK_PKG_CONFIG_ENV "PKG_CONFIG_PATH=${DXVK_PKG_CONFIG_PATH}")

  if(SAGE_DXVK_USE_LOCAL_FORK AND EXISTS "${DXVK_LOCAL_FORK_DIR}/.git")
    ExternalProject_Add(dxvk_macos_build
      # GeneralsX: Build from local fbraz3 fork to avoid stale remote hash pins.
      SOURCE_DIR        ${DXVK_SOURCE_DIR}
      BINARY_DIR        ${DXVK_BUILD_DIR}
      DOWNLOAD_COMMAND  ""
      UPDATE_COMMAND    ""
      PATCH_COMMAND     ""
      CONFIGURE_COMMAND ${CMAKE_COMMAND} -E env CC=clang CXX=clang++ "CFLAGS=-arch ${DXVK_HOST_ARCH} -mcpu=apple-m1" "CXXFLAGS=-arch ${DXVK_HOST_ARCH} -mcpu=apple-m1" "LDFLAGS=-arch ${DXVK_HOST_ARCH}" "${DXVK_PKG_CONFIG_ENV}" ${VULKAN_SDK_ENV_VAR} ${MESON_EXECUTABLE} setup ${DXVK_BUILD_DIR} ${DXVK_SOURCE_DIR} ${DXVK_MESON_MACHINE_ARGS} -Ddxvk_native_wsi=sdl3 --buildtype=release --reconfigure
      BUILD_COMMAND     ${NINJA_EXECUTABLE} -C ${DXVK_BUILD_DIR} src/d3d9/libdxvk_d3d9.0.dylib src/d3d8/libdxvk_d3d8.0.dylib
      INSTALL_COMMAND   ""
      UPDATE_DISCONNECTED TRUE
    )
  else()
    # GeneralsX: Pin remote DXVK to immutable commit produced by fix/macos-size_t-cstddef.
    set(DXVK_REMOTE_REF 46a3bc018bcae408d49d3c500e4e536a11f6789a)
    ExternalProject_Add(dxvk_macos_build
      # GeneralsX: Consume pre-patched source from pinned fork commit.
      GIT_REPOSITORY    https://github.com/fbraz3/dxvk.git
      GIT_TAG           ${DXVK_REMOTE_REF}
      # GeneralsX: Keep pinned commit fetch reliable across clean CI builds.
      GIT_SHALLOW       FALSE
      SOURCE_DIR        ${DXVK_SOURCE_DIR}
      BINARY_DIR        ${DXVK_BUILD_DIR}
      PATCH_COMMAND     ""
      CONFIGURE_COMMAND ${CMAKE_COMMAND} -E env CC=clang CXX=clang++ "CFLAGS=-arch ${DXVK_HOST_ARCH} -mcpu=apple-m1" "CXXFLAGS=-arch ${DXVK_HOST_ARCH} -mcpu=apple-m1" "LDFLAGS=-arch ${DXVK_HOST_ARCH}" "${DXVK_PKG_CONFIG_ENV}" ${VULKAN_SDK_ENV_VAR} ${MESON_EXECUTABLE} setup ${DXVK_BUILD_DIR} ${DXVK_SOURCE_DIR} ${DXVK_MESON_MACHINE_ARGS} -Ddxvk_native_wsi=sdl3 --buildtype=release --reconfigure
      BUILD_COMMAND     ${NINJA_EXECUTABLE} -C ${DXVK_BUILD_DIR} src/d3d9/libdxvk_d3d9.0.dylib src/d3d8/libdxvk_d3d8.0.dylib
      INSTALL_COMMAND   ""
      UPDATE_DISCONNECTED FALSE
    )
  endif()

  # Copy libdxvk_d3d9 + libdxvk_d3d8 to build dir and create unversioned symlinks.
  # d3d8 links against d3d9 via @rpath, so both must be present at runtime.
  add_custom_command(
    OUTPUT  "${CMAKE_BINARY_DIR}/libdxvk_d3d9.0.dylib"
            "${CMAKE_BINARY_DIR}/libdxvk_d3d8.0.dylib"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
              ${DXVK_D3D9_LIB} "${CMAKE_BINARY_DIR}/libdxvk_d3d9.0.dylib"
    COMMAND ${CMAKE_COMMAND} -E create_symlink
              libdxvk_d3d9.0.dylib "${CMAKE_BINARY_DIR}/libdxvk_d3d9.dylib"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
              ${DXVK_D3D8_LIB} "${CMAKE_BINARY_DIR}/libdxvk_d3d8.0.dylib"
    COMMAND ${CMAKE_COMMAND} -E create_symlink
              libdxvk_d3d8.0.dylib "${CMAKE_BINARY_DIR}/libdxvk_d3d8.dylib"
    DEPENDS dxvk_macos_build
    COMMENT "Installing libdxvk_d3d8 + libdxvk_d3d9 to build directory"
  )
  add_custom_target(dxvk_d3d8_install ALL
    DEPENDS "${CMAKE_BINARY_DIR}/libdxvk_d3d8.0.dylib"
            "${CMAKE_BINARY_DIR}/libdxvk_d3d9.0.dylib"
  )

  # Export path so other cmake files know where the headers are
  set(DXVK_INCLUDE_DIR "${DXVK_SOURCE_DIR}/include/native" CACHE PATH "DXVK native headers")
  # GeneralsX @build felipebraz 10/06/2025 Mirror lowercase dxvk_SOURCE_DIR that FetchContent sets on Linux
  # so CompatLib/CMakeLists.txt check works on macOS as well (CACHE PATH survives auto-regeneration)
  set(dxvk_SOURCE_DIR "${DXVK_SOURCE_DIR}" CACHE PATH "DXVK source directory (macOS)")
  message(STATUS "DXVK source directory: ${DXVK_SOURCE_DIR}")
  message(STATUS "DXVK d3d8 library:     ${DXVK_D3D8_LIB}")

elseif(WIN32)
  # GeneralsX: Fail loudly instead of falling through to the Linux arm.
  # Reaching here means someone forced -DSAGE_USE_DX8=OFF on a Windows target. The arm below would
  # then put DXVK's Linux <windows.h>/<windows_base.h> on the include path of every translation
  # unit and shadow the real Windows SDK. A silent fallthrough is not a configuration
  # anyone can build.
  message(FATAL_ERROR
    "SAGE_USE_DX8=OFF is not a supported Windows configuration.\n"
    "DXVK's native tarball is a Linux package: its include/dxvk ships DXVK's own windows.h and "
    "windows_base.h, which shadow the Windows SDK and break every translation unit.\n"
    "On Windows, DXVK is a drop-in d3d8.dll/d3d9.dll placed next to the game exe - it contributes "
    "no headers. Build with SAGE_USE_DX8=ON (the default) so the DirectX 8 headers come from "
    "min-dx8-sdk.")

else()
  # Linux: Fetch pre-built DXVK native binary for DirectX→Vulkan translation
  # Native 32-bit and 64-bit Linux binaries (.so)
  FetchContent_Declare(
    dxvk
    URL        https://github.com/doitsujin/dxvk/releases/download/v2.6/dxvk-native-2.6-steamrt-sniper.tar.gz
  )
  FetchContent_MakeAvailable(dxvk)
  message(STATUS "Using DXVK native (Linux DirectX→Vulkan)")
  message(STATUS "DXVK source directory: ${dxvk_SOURCE_DIR}")
endif()
