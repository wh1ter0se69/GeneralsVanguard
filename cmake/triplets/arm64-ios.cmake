# Overlay triplet: pin the iOS deployment target so vcpkg-built static libs
# (freetype, curl, openal, ...) match the engine's CMAKE_OSX_DEPLOYMENT_TARGET.
# Without this, vcpkg builds against the host SDK's default (e.g. iOS 26.5) and
# the final link warns "built for newer iOS version than being linked (16.0)" —
# and unguarded newer-API usage inside those libs can crash on older devices.
set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_CMAKE_SYSTEM_NAME iOS)
set(VCPKG_OSX_DEPLOYMENT_TARGET "16.0")
set(VCPKG_OSX_ARCHITECTURES arm64)
#
# GeneralsX @build Claude 18/09/2026 THE SDK IS PINNED BY NAME. Without VCPKG_OSX_SYSROOT the port
# build takes whatever SDK the compiler finds by default, and on a host whose OS is newer than its
# Xcode that is the Command Line Tools' SDK for the NEWER OS - which the older Xcode linker cannot
# read ("tapi error: malformed file ... unknown architecture"). Measured 18/09/2026: a MacBook on
# macOS 27.0 with Xcode 26.6 failed both Apple legs of verify-4platform.sh this way. A NAME (not a
# path) is resolved by CMake through `xcrun --sdk <name>`, i.e. through the ACTIVE Xcode, so it is
# right on every machine and pins nothing machine-specific. Setting SDKROOT in the environment is
# NOT a substitute: it also reached the iOS triplet's port builds and produced a freetype built for
# macCatalyst, which the iOS link then refused.
set(VCPKG_OSX_SYSROOT "iphoneos")
