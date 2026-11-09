# Create gitinfo.cpp
set(GIT_PRE_CONFIGURE_FILE "${CMAKE_CURRENT_SOURCE_DIR}/gitinfo/gitinfo.cpp.in")
set(GIT_POST_CONFIGURE_FILE "${CMAKE_CURRENT_BINARY_DIR}/gitinfo.cpp")

# GeneralsX @feature Claude 27/07/2026 Create simsourcedigest.cpp for the join-time
# SimID compatibility check. Reuses the gitinfo directory so no new include path or
# link edge is needed - it is already PUBLIC on resources, which corei_always links.
set(SIMDIGEST_PRE_CONFIGURE_FILE "${CMAKE_CURRENT_SOURCE_DIR}/gitinfo/simsourcedigest.cpp.in")
set(SIMDIGEST_POST_CONFIGURE_FILE "${CMAKE_CURRENT_BINARY_DIR}/simsourcedigest.cpp")

include(gitinfo/git_watcher.cmake)
include(gitinfo/simsourcedigest_watcher.cmake)

# Create resources library
add_library(resources STATIC)

target_sources(resources PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}/gitinfo.cpp"
    gitinfo/gitinfo.h
    "${CMAKE_CURRENT_BINARY_DIR}/simsourcedigest.cpp"
    gitinfo/simsourcedigest.h
)

target_include_directories(resources PUBLIC
    gitinfo
)

# GeneralsX @feature Claude 27/07/2026 Unlike check_git, make the ordering edge
# explicit: the generated .cpp must exist before resources compiles, and BYPRODUCTS
# alone does not create that edge under Ninja.
add_dependencies(resources check_sim_source_digest)

# Add resources (optional)
if (MSVC_IDE)
    target_sources(resources PRIVATE
        visualstudio/asciistring.natvis
        visualstudio/stllist.natvis
        visualstudio/stlvector.natvis
        visualstudio/unicodestring.natvis
    )
endif()
