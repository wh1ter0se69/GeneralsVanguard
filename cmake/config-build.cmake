# Do we want to build extra SDK stuff or just the game binary?
option(RTS_BUILD_CORE_TOOLS "Build core tools" ON)
option(RTS_BUILD_CORE_EXTRAS "Build core extra tools/tests" OFF)
option(RTS_BUILD_ZEROHOUR "Build Zero Hour code." ON)
option(RTS_BUILD_GENERALS "Build Generals code." ON)
option(RTS_BUILD_OPTION_PROFILE "Build code with the \"Profile\" configuration." OFF)
option(RTS_BUILD_OPTION_PROFILE_TRACY "Build code with Tracy profiling enabled." OFF)
option(RTS_BUILD_OPTION_DEBUG "Build code with the \"Debug\" configuration." OFF)
option(RTS_BUILD_OPTION_ASAN "Build code with Address Sanitizer." OFF)
option(RTS_BUILD_OPTION_VC6_FULL_DEBUG "Build VC6 with full debug info." OFF)
option(RTS_BUILD_OPTION_FFMPEG "Enable FFmpeg support" OFF)

# Linux/SDL3 and OpenAL options (Phase 1 Linux port)
option(SAGE_USE_SDL3 "Use SDL3 for windowing/input (Linux/macOS)" OFF)
option(SAGE_USE_OPENAL "Use OpenAL for audio backend (Linux/macOS)" OFF)

# GeneralsX: In-game update checker via GitHub Releases API (SDL3+libcurl builds only)
# Default ON when SDL3 is enabled, but only if the user has not explicitly set SAGE_UPDATE_CHECK.
# An explicit -DSAGE_UPDATE_CHECK=OFF on the cmake command line is always respected.
if(NOT DEFINED CACHE{SAGE_UPDATE_CHECK})
    set(SAGE_UPDATE_CHECK "${SAGE_USE_SDL3}" CACHE BOOL "Enable in-game update check via GitHub Releases API")
endif()

# macOS port option (Phase 5)
option(SAGE_USE_MOLTENVK "Use MoltenVK for Vulkan on macOS (Phase 5 macOS port)" OFF)

# SagePatch — optional QoL features for casual play (screenshot, cursor lock,
# brightness, camera/scroll INI overrides). Compiles to a separate shared lib
# that is loaded via DYLD_INSERT_LIBRARIES (macOS) / LD_PRELOAD (Linux) at runtime.
option(RTS_BUILD_OPTION_SAGE_PATCH "Build SagePatch QoL extras (macOS/Linux, requires SDL3)" ON)

if(NOT RTS_BUILD_ZEROHOUR AND NOT RTS_BUILD_GENERALS)
    set(RTS_BUILD_ZEROHOUR TRUE)
    message("You must select one project to build, building Zero Hour by default.")
endif()

add_feature_info(CoreTools RTS_BUILD_CORE_TOOLS "Build Core Mod Tools")
add_feature_info(CoreExtras RTS_BUILD_CORE_EXTRAS "Build Core Extra Tools/Tests")
add_feature_info(ZeroHourStuff RTS_BUILD_ZEROHOUR "Build Zero Hour code")
add_feature_info(GeneralsStuff RTS_BUILD_GENERALS "Build Generals code")
add_feature_info(ProfileBuild RTS_BUILD_OPTION_PROFILE "Building as a \"Profile\" build")
add_feature_info(DebugBuild RTS_BUILD_OPTION_DEBUG "Building as a \"Debug\" build")
add_feature_info(AddressSanitizer RTS_BUILD_OPTION_ASAN "Building with address sanitizer")
add_feature_info(Vc6FullDebug RTS_BUILD_OPTION_VC6_FULL_DEBUG "Building VC6 with full debug info")
add_feature_info(FFmpegSupport RTS_BUILD_OPTION_FFMPEG "Building with FFmpeg support")
add_feature_info(SDL3Windowing SAGE_USE_SDL3 "Using SDL3 for windowing (Linux)")
add_feature_info(OpenALAudio SAGE_USE_OPENAL "Using OpenAL for audio (Linux)")
add_feature_info(UpdateCheck SAGE_UPDATE_CHECK "In-game update check via GitHub Releases API")
add_feature_info(SagePatch RTS_BUILD_OPTION_SAGE_PATCH "Build SagePatch QoL extras (macOS)")

set(RTS_BUILD_OUTPUT_SUFFIX "" CACHE STRING "Suffix appended to output names of installable targets")

if(RTS_BUILD_ZEROHOUR)
    option(RTS_BUILD_ZEROHOUR_TOOLS "Build tools for Zero Hour" ON)
    option(RTS_BUILD_ZEROHOUR_EXTRAS "Build extra tools/tests for Zero Hour" OFF)
    option(RTS_BUILD_ZEROHOUR_DOCS "Build documentation for Zero Hour" OFF)

    add_feature_info(ZeroHourTools RTS_BUILD_ZEROHOUR_TOOLS "Build Zero Hour Mod Tools")
    add_feature_info(ZeroHourExtras RTS_BUILD_ZEROHOUR_EXTRAS "Build Zero Hour Extra Tools/Tests")
    add_feature_info(ZeroHourDocs RTS_BUILD_ZEROHOUR_DOCS "Build Zero Hour Documentation")
endif()

if(RTS_BUILD_GENERALS)
    option(RTS_BUILD_GENERALS_TOOLS "Build tools for Generals" ON)
    option(RTS_BUILD_GENERALS_EXTRAS "Build extra tools/tests for Generals" OFF)
    option(RTS_BUILD_GENERALS_DOCS "Build documentation for Generals" OFF)

    add_feature_info(GeneralsTools RTS_BUILD_GENERALS_TOOLS "Build Generals Mod Tools")
    add_feature_info(GeneralsExtras RTS_BUILD_GENERALS_EXTRAS "Build Generals Extra Tools/Tests")
    add_feature_info(GeneralsDocs RTS_BUILD_GENERALS_DOCS "Build Generals Documentation")
endif()

if(NOT IS_VS6_BUILD)
    # Because we set CMAKE_CXX_STANDARD_REQUIRED and CMAKE_CXX_EXTENSIONS in the compilers.cmake this should be enforced.
    target_compile_features(core_config INTERFACE cxx_std_20)
endif()

if(IS_VS6_BUILD AND RTS_BUILD_OPTION_VC6_FULL_DEBUG)
    target_compile_options(core_config INTERFACE ${RTS_FLAGS} /Zi)
else()
    target_compile_options(core_config INTERFACE ${RTS_FLAGS})
endif()

# This disables a lot of warnings steering developers to use windows only functions/function names.
if(MSVC)
    target_compile_definitions(core_config INTERFACE _CRT_NONSTDC_NO_WARNINGS _CRT_SECURE_NO_WARNINGS $<$<CONFIG:DEBUG>:_DEBUG_CRT>)
endif()

if(UNIX)
    target_compile_definitions(core_config INTERFACE _UNIX)

    # Detect these instead of assuming them: glibc only has strlcpy/strlcat from 2.38 and never has wcslcpy/wcslcat.
    # Defining HAVE_* unconditionally suppresses the portable fallbacks in WWLib/stringex.h (guarded by #ifndef HAVE_*)
    # and breaks the Linux build. A compile check is correct on every libc.
    include(CheckSymbolExists)
    check_symbol_exists(strlcpy "string.h" GX_LIBC_HAS_STRLCPY)
    check_symbol_exists(strlcat "string.h" GX_LIBC_HAS_STRLCAT)
    check_symbol_exists(wcslcpy "wchar.h"  GX_LIBC_HAS_WCSLCPY)
    check_symbol_exists(wcslcat "wchar.h"  GX_LIBC_HAS_WCSLCAT)

    if(GX_LIBC_HAS_STRLCPY)
        target_compile_definitions(core_config INTERFACE HAVE_STRLCPY)
    endif()
    if(GX_LIBC_HAS_STRLCAT)
        target_compile_definitions(core_config INTERFACE HAVE_STRLCAT)
    endif()
    if(GX_LIBC_HAS_WCSLCPY)
        target_compile_definitions(core_config INTERFACE HAVE_WCSLCPY)
    endif()
    if(GX_LIBC_HAS_WCSLCAT)
        target_compile_definitions(core_config INTERFACE HAVE_WCSLCAT)
    endif()
endif()

if(RTS_BUILD_OPTION_DEBUG)
    target_compile_definitions(core_config INTERFACE RTS_DEBUG WWDEBUG DEBUG)
else()
    target_compile_definitions(core_config INTERFACE RTS_RELEASE NDEBUG)
endif()

if(RTS_BUILD_OPTION_PROFILE)
    target_compile_definitions(core_config INTERFACE RTS_PROFILE_LEGACY)
endif()

# Define a dummy Tracy target when the build option is disabled.
if(RTS_BUILD_OPTION_PROFILE_TRACY)
    include(cmake/tracy.cmake)
else()
    add_library(core_profile_tracy INTERFACE)
endif()
# Linux port options (Phase 1)
if(SAGE_USE_SDL3)
    target_compile_definitions(core_config INTERFACE SAGE_USE_SDL3)
    message(STATUS "SDL3 windowing backend enabled")
endif()

if(SAGE_USE_OPENAL)
    target_compile_definitions(core_config INTERFACE SAGE_USE_OPENAL)
    message(STATUS "OpenAL audio backend enabled")
endif()

# Opt-in 60 Hz simulation tick. sourceID hashes the source, so 30 Hz and 60 Hz builds of one commit share it; engineID
# hashes LOGICFRAMES_PER_SECOND, so a mismatched pair is refused with ENGINE_DIFFERS. Do not add the flag to the digest.
# Defaults OFF but every shipping preset sets it ON; keep presets and deployed fleet in agreement.
# At 60 Hz about 20 gameplay constants are raw frame counts and halve in duration.
option(GX_LOGIC_60HZ "Run the simulation at 60 Hz instead of 30 (see the note above - ON in every shipping preset)" OFF)
if(GX_LOGIC_60HZ)
    target_compile_definitions(core_config INTERFACE GX_LOGIC_60HZ=1)
    message(WARNING "GX_LOGIC_60HZ: simulation tick is 60 Hz. Incompatible with 30 Hz peers (refused at engineID) and with the 30 Hz determinism corpus.")
endif()

# GeneralsX: The GX_TEST_FORGE network instrument, compiled in only on request.
#
# Network::gxTestForge makes a peer SEND forged or out-of-turn kill-path orders for
# scripts/test/net-kill-gates.sh. Dormant is not safe enough to ship: from the packet router its
# ejectdestroy kind kicks and defeats any live peer with nothing but an environment variable, and
# environment variables are not part of SimID. A modified client could already do that; a stock
# binary must not. OFF in every shipping build - the harness reports NOT RUN against one.
option(GX_TEST_INSTRUMENTS "Compile in the GX_TEST_FORGE network test instrument - never in a shipping build" OFF)
if(GX_TEST_INSTRUMENTS)
    target_compile_definitions(core_config INTERFACE GX_TEST_INSTRUMENTS=1)
    message(WARNING "GX_TEST_INSTRUMENTS: GX_TEST_FORGE is compiled in. Never ship this build.")
endif()

# GeneralsX: The A* cost overflow counter (cmax=/cwrap= on [GXPATH], audit S10-F10),
# compiled in only for a measurement build: it sits in the A* cell-expansion path. Observation only - it changes
# no simulation state - but a shipping tick does not pay for a counter nobody reads. OFF in every preset.
option(GX_PATHCOST_COUNT "Compile in the A* cost overflow counter (cmax=/cwrap= on [GXPATH]) - measurement builds only" OFF)
if(GX_PATHCOST_COUNT)
    target_compile_definitions(core_config INTERFACE GX_PATHCOST_COUNT=1)
endif()

# GeneralsX: Update check compile definition
if(SAGE_UPDATE_CHECK)
    target_compile_definitions(core_config INTERFACE SAGE_UPDATE_CHECK)
    message(STATUS "In-game update checker enabled")
endif()

if(SAGE_USE_GLM)
    target_compile_definitions(core_config INTERFACE SAGE_USE_GLM)
    message(STATUS "GLM math library enabled (DirectX 8 replacement)")
endif()

# macOS MoltenVK detection (Phase 5)
# GeneralsX: Phase 5 macOS port
if(APPLE AND SAGE_USE_MOLTENVK)
    find_package(Vulkan REQUIRED COMPONENTS MoltenVK)
    if(NOT Vulkan_FOUND)
        message(FATAL_ERROR "MoltenVK not found. Install from https://vulkan.lunarg.com/sdk/home#mac (LunarG installer to ~/VulkanSDK/<version>/macOS)")
    endif()
    
    target_compile_definitions(core_config INTERFACE SAGE_USE_MOLTENVK)
    target_link_libraries(core_config INTERFACE Vulkan::Vulkan)
    if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
        # On iOS, MoltenVK links as a STATIC library, so its framework dependencies
        # must be linked by the consumer (the macOS dylib resolved these itself).
        target_link_libraries(core_config INTERFACE
            "-framework Metal"
            "-framework IOSurface"
            "-framework CoreGraphics"
            "-framework QuartzCore"
            "-framework Foundation"
            "-framework UIKit"
        )

        # libMoltenVK.a is -force_load ed because no engine translation unit references Vulkan directly (SDL3 and DXVK reach
        # it at runtime), so the archive would be dropped and the executable would export no vk symbols. SDL3's UIKit
        # loader then finds neither vkGetInstanceProcAddr via dlsym nor a libvulkan.dylib to dlopen, and startup fails.
        # DXVK is unaffected: Patches/dxvk-ios.patch gives it the bundle-relative framework path.
        set(_gx_moltenvk_archive "${Vulkan_MoltenVK_LIBRARY}")
        if(NOT _gx_moltenvk_archive)
            set(_gx_moltenvk_archive "${Vulkan_LIBRARY}")
        endif()
        if(NOT _gx_moltenvk_archive MATCHES "\\.a$")
            message(FATAL_ERROR
                "iOS build expects a STATIC MoltenVK archive so its symbols land in the executable, "
                "but Vulkan_MoltenVK_LIBRARY/Vulkan_LIBRARY resolved to '${_gx_moltenvk_archive}'. "
                "Configure with `cmake --preset ios-vulkan` (a plain -S/-B drops the preset's "
                "Vulkan_* cache entries) and export VULKAN_SDK to the LunarG macOS SDK.")
        endif()
        target_link_options(core_config INTERFACE "LINKER:-force_load,${_gx_moltenvk_archive}")
        message(STATUS "  MoltenVK: force-loading ${_gx_moltenvk_archive} (SDL3 UIKit needs vkGetInstanceProcAddr in the executable)")
    endif()
    message(STATUS "MoltenVK (Vulkan on macOS) detected and enabled")
    message(STATUS "  Vulkan SDK: ${Vulkan_INCLUDE_DIRS}")
    message(STATUS "  MoltenVK: Will translate Vulkan to Metal")
endif()