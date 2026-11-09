# GeneralsX @build BenderAI 29/05/2025
# ccache compiler cache support
#
# Significantly speeds up recompilation by caching object files.
# Auto-detects ccache installation; no-op if not found.
#
# Reference pattern from old-multiplatform-attempt branch:
#   CMAKE_C_COMPILER_LAUNCHER   = ccache
#   CMAKE_CXX_COMPILER_LAUNCHER = ccache

option(SAGE_USE_CCACHE "Use ccache compiler cache if available" ON)

if(SAGE_USE_CCACHE)
    find_program(CCACHE_PROGRAM ccache)
    if(CCACHE_PROGRAM)
        set(CMAKE_C_COMPILER_LAUNCHER   "${CCACHE_PROGRAM}" CACHE STRING "C compiler launcher")
        set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}" CACHE STRING "C++ compiler launcher")
        message(STATUS "ccache enabled: ${CCACHE_PROGRAM}")
        
        # GeneralsX @build BenderAI 25/02/2026
        # Use CCACHE_SLOPPINESS env var so we don't mutate the global ccache config
        # as a side-effect of CMake configure.  The env var is inherited by compiler
        # invocations launched through CMAKE_<LANG>_COMPILER_LAUNCHER.
        if(APPLE)
            set(ENV{CCACHE_SLOPPINESS} "time_macros,locale")
            message(STATUS "ccache: CCACHE_SLOPPINESS=time_macros,locale set for this build")
        endif()
    else()
        message(STATUS "ccache not found, building without compiler cache")
    endif()
else()
    # GeneralsX @bugfix Claude 27/07/2026 Actually disable it.
    #
    # The ON branch above sets these with CACHE STRING, so they PERSIST in CMakeCache.txt.
    # Printing "ccache disabled" while leaving CMAKE_CXX_COMPILER_LAUNCHER pointing at
    # ccache.exe made -DSAGE_USE_CCACHE=OFF a no-op on any pre-existing build directory:
    # the configure log and the cache said opposite things. That matters more than a stale
    # flag normally would, because this option is the tool used to prove that a rebuild
    # genuinely recompiled rather than replaying a cache - so the bug quietly weakened the
    # one check that exists against a fake "clean rebuild".
    unset(CMAKE_C_COMPILER_LAUNCHER CACHE)
    unset(CMAKE_CXX_COMPILER_LAUNCHER CACHE)
    message(STATUS "ccache disabled (SAGE_USE_CCACHE=OFF), compiler launchers cleared")
endif()
