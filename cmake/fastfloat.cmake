# GeneralsX @bugfix Claude 25/09/2026 fast_float: the one INI number parser on every platform
# (Common/INIScanNumber.h). vcpkg provides it on the vcpkg presets; the presets without vcpkg
# (mingw-w64-i686, win32) and the Flatpak fetch the same release instead, like cmake/stb.cmake.
#
# The version is pinned EXACT on both paths because it is a determinism input: a different
# fast_float could round or accept an INI token differently and split the simulation between
# peers. Bump the vcpkg builtin-baseline, this version and the tag below together.
find_package(FastFloat 8.0.2 EXACT CONFIG QUIET)

if(NOT FastFloat_FOUND)
	include(FetchContent)
	FetchContent_Declare(
		fast_float
		GIT_REPOSITORY https://github.com/fastfloat/fast_float.git
		GIT_TAG        50a80a73ab2ab256ba1c3bf86923ddd8b4202bc7 # v8.0.2
		# Header-only: populate the source, but do not add_subdirectory() its CMakeLists.txt,
		# which would force CMAKE_BUILD_TYPE when it is empty and add install rules.
		SOURCE_SUBDIR  do-not-add
	)
	FetchContent_MakeAvailable(fast_float)

	add_library(FastFloat::fast_float INTERFACE IMPORTED)
	target_include_directories(FastFloat::fast_float INTERFACE ${fast_float_SOURCE_DIR}/include)
	message(STATUS "fast_float: vcpkg package not found, using the fetched v8.0.2 at ${fast_float_SOURCE_DIR}")
endif()
