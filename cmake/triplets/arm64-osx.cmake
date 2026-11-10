# GeneralsX: vcpkg overlay triplet for Apple Silicon
# Overrides the built-in arm64-osx triplet to pin VCPKG_OSX_DEPLOYMENT_TARGET,
# ensuring that vcpkg-installed packages (fontconfig, freetype, libpng, bz2, etc.)
# are compiled with the same macOS minimum version as the game itself (15.0).
# Without this the built-in triplet inherits the host SDK version (e.g. 26.0),
# producing hundreds of ld warnings: "was built for newer macOS version (26.0)
# than being linked (15.0)".
#
# AFTER changing this triplet, the installed vcpkg packages must be rebuilt:
#   rm -rf build/macos-vulkan/vcpkg_installed
#   cmake --preset macos-vulkan

set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_CMAKE_SYSTEM_NAME Darwin)
set(VCPKG_OSX_ARCHITECTURES arm64)
set(VCPKG_OSX_DEPLOYMENT_TARGET "15.0")
#
# GeneralsX: THE SDK IS PINNED BY NAME. Without VCPKG_OSX_SYSROOT the port
# build takes whatever SDK the compiler finds by default, and on a host whose OS is newer than its
# Xcode that is the Command Line Tools' SDK for the NEWER OS - which the older Xcode linker cannot
# read ("tapi error: malformed file ... unknown architecture"). Measured 18/09/2026: a MacBook on
# macOS 27.0 with Xcode 26.6 failed both Apple legs of verify-4platform.sh this way. A NAME (not a
# path) is resolved by CMake through `xcrun --sdk <name>`, i.e. through the ACTIVE Xcode, so it is
# right on every machine and pins nothing machine-specific. Setting SDKROOT in the environment is
# NOT a substitute: it also reached the iOS triplet's port builds and produced a freetype built for
# macCatalyst, which the iOS link then refused.
set(VCPKG_OSX_SYSROOT "macosx")
