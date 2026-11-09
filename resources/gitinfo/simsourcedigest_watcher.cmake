# simsourcedigest_watcher.cmake
#
# GeneralsX @feature Claude 27/07/2026
# Build-time 32-bit digest of the simulation-relevant source tree, used by the
# join-time SimID compatibility check (Common/Diagnostic/SimulationId.h).
#
# Structured exactly like its sibling gitinfo/git_watcher.cmake, and for the same
# reason: this file is invoked twice, once at configure time (to register the custom
# target) and once per build via `cmake -P`. In script mode CMake starts a fresh
# process in which CMAKE_SOURCE_DIR is the *invoking process's working directory* -
# for Ninja that is ${sourceDir}/build/<preset>/resources, not the repo root - and no
# find_package(Git) has run, so GIT_EXECUTABLE is empty. Every value the build-time
# half needs is therefore forwarded with -D on the custom command, and nothing below
# Main() may reference CMAKE_SOURCE_DIR or CMAKE_CURRENT_BINARY_DIR.
#
# WINDOWS HOOK (not solved here; Apple targets only in this pass):
#   A Windows clone made with Git for Windows' installer default core.autocrlf=true
#   has a CRLF working tree. `git hash-object` applies the checkin filter by default,
#   so under plain autocrlf a re-saved file still hashes to the committed LF blob and
#   the "only record it if the sha differs" rule below discards it. Do NOT add a root
#   .gitattributes containing `* -text`: -text DISABLES that filter and would make
#   every file in a CRLF working tree land in the overlay, denying every Windows join
#   for a semantically identical tree. If a .gitattributes is wanted, it must be
#   `* text=auto eol=lf` together with a one-time `git add --renormalize .`.

# Short hand for converting paths to absolute.
macro(PATH_TO_ABSOLUTE var_name)
    get_filename_component(${var_name} "${${var_name}}" ABSOLUTE)
endmacro()

# Check that a required variable is set.
macro(CHECK_REQUIRED_VARIABLE var_name)
    if(NOT DEFINED ${var_name})
        message(FATAL_ERROR "The \"${var_name}\" variable must be defined.")
    endif()
    PATH_TO_ABSOLUTE(${var_name})
endmacro()

# Check that an optional variable is set, or, set it to a default value.
macro(CHECK_OPTIONAL_VARIABLE var_name default_value)
    if(NOT DEFINED ${var_name})
        set(${var_name} ${default_value})
    endif()
    PATH_TO_ABSOLUTE(${var_name})
endmacro()

CHECK_REQUIRED_VARIABLE(SIMDIGEST_PRE_CONFIGURE_FILE)
CHECK_REQUIRED_VARIABLE(SIMDIGEST_POST_CONFIGURE_FILE)
CHECK_OPTIONAL_VARIABLE(SIMDIGEST_WORKING_DIR "${CMAKE_SOURCE_DIR}")

if(NOT DEFINED GIT_EXECUTABLE)
    find_package(Git QUIET)
endif()

# ---------------------------------------------------------------------------
# Directory pathspecs only; the extension filter is applied afterwards in CMake,
# NOT as a git pathspec. `git ls-tree` does not run the full pathspec engine - a
# bare '*.cpp' term there matches zero files - and mixing a directory term with a
# glob term ORs them rather than intersecting. Filtering the output lines gives the
# intended intersection and applies identically to ls-tree, diff-index and ls-files,
# so untracked debris such as an editor backup cannot move the digest.
# ---------------------------------------------------------------------------
# NO CLOSING PARENTHESIS INSIDE THE SIMDIGEST_PATHS_* LISTS BELOW, comments included: scripts/tooling/tsh-adoption.py,
# scripts/test/stress-option-refusal-test.sh and tools/relay/test-perf.js read them with a regex that ends the list at
# the first one, so every path after it silently drops out of their view (02/10/2026: twelve CMakeLists.txt did).
set(SIMDIGEST_CODE_EXT_REGEX "\\.(c|cc|cpp|cxx|h|hh|hpp|inl)$")
set(SIMDIGEST_BUILD_EXT_REGEX "\\.(cmake|json|in|ini|py|txt)$")

set(SIMDIGEST_PATHS_CODE_SHARED
    Core/GameEngine/Include/Common
    Core/GameEngine/Include/GameLogic
    Core/GameEngine/Include/GameNetwork
    Core/GameEngine/Source/Common
    Core/GameEngine/Source/GameLogic
    Core/GameEngine/Source/GameNetwork
    # Core/Libraries/Include is where this fork's own documented ungated divergences
    # live: Lib/BaseType.h REAL_TO_INT and the Lib/trig.h Sqrt gateway.
    Core/Libraries/Include
    Core/Libraries/Source/WWVegas/WWMath
    # GeneralsX @bugfix Claude 03/10/2026 Codex P2 det-codex-fixes: device and client files whose code the SIMULATION
    # reads, digested whole because they are small or rarely edited - every -headless peer runs them too, through
    # W3DGameLogic. Hot render files with only a few such functions are on the SIMDIGEST_PATHS_REGION_* lists below.
    # Terrain: the map parse - heights, cliff bits, boundaries, the MapObject list, time of day, terrain types for
    # build legality - the bridge INI tables, and the visual/water classes logic queries through.
    Core/GameEngineDevice/Source/W3DDevice/GameClient/WorldHeightMap.cpp
    Core/GameEngineDevice/Include/W3DDevice/GameClient/WorldHeightMap.h
    Core/GameEngineDevice/Include/W3DDevice/GameClient/W3DTerrainVisual.h
    Core/GameEngineDevice/Include/W3DDevice/GameClient/W3DWater.h
    Core/GameEngine/Source/GameClient/Terrain/TerrainRoads.cpp
    Core/GameEngine/Include/GameClient/TerrainRoads.h
    Core/GameEngine/Source/GameClient/Terrain/TerrainVisual.cpp
    Core/GameEngine/Include/GameClient/TerrainVisual.h
    # Bones, barrels and launch offsets: the model-draw module data, the laser width ParticleUplinkCannon reads, and
    # the WW3D2 pose code that turns a model and its first animation frame into the pristine bone positions.
    Core/GameEngineDevice/Include/W3DDevice/GameClient/Module/W3DModelDraw.h
    Core/GameEngineDevice/Include/W3DDevice/GameClient/Module/W3DLaserDraw.h
    Core/Libraries/Source/WWVegas/WW3D2/htree.cpp
    Core/Libraries/Source/WWVegas/WW3D2/htree.h
    Core/Libraries/Source/WWVegas/WW3D2/hcanim.cpp
    Core/Libraries/Source/WWVegas/WW3D2/hcanim.h
    Core/Libraries/Source/WWVegas/WW3D2/hanim.cpp
    Core/Libraries/Source/WWVegas/WW3D2/hanim.h
    Core/Libraries/Source/WWVegas/WW3D2/hmdldef.cpp
    Core/Libraries/Source/WWVegas/WW3D2/hmdldef.h
    Core/Libraries/Source/WWVegas/WW3D2/pivot.cpp
    Core/Libraries/Source/WWVegas/WW3D2/pivot.h
    Core/Libraries/Source/WWVegas/WW3D2/rendobj.cpp
    Core/Libraries/Source/WWVegas/WW3D2/rendobj.h
    Core/Libraries/Source/WWVegas/WW3D2/proto.cpp
    Core/Libraries/Source/WWVegas/WW3D2/proto.h
    # det-codex r8: every render-object class a model can contain - the prototype loaders WW3DAssetManager registers,
    # and what aggregates attach. RenderObjClass::Get_Sub_Object_By_Name, which the bone fallback reaches, calls their
    # virtual Get_Name, and collections and distance LODs answer the sub-object and bone queries themselves.
    Core/Libraries/Source/WWVegas/WW3D2/composite.cpp
    Core/Libraries/Source/WWVegas/WW3D2/composite.h
    Core/Libraries/Source/WWVegas/WW3D2/collect.cpp
    Core/Libraries/Source/WWVegas/WW3D2/collect.h
    Core/Libraries/Source/WWVegas/WW3D2/distlod.cpp
    Core/Libraries/Source/WWVegas/WW3D2/distlod.h
    Core/Libraries/Source/WWVegas/WW3D2/agg_def.cpp
    Core/Libraries/Source/WWVegas/WW3D2/agg_def.h
    Core/Libraries/Source/WWVegas/WW3D2/ringobj.cpp
    Core/Libraries/Source/WWVegas/WW3D2/ringobj.h
    Core/Libraries/Source/WWVegas/WW3D2/sphereobj.cpp
    Core/Libraries/Source/WWVegas/WW3D2/sphereobj.h
    Core/Libraries/Source/WWVegas/WW3D2/soundrobj.cpp
    Core/Libraries/Source/WWVegas/WW3D2/soundrobj.h
    # det-codex-5: constants-only headers the digested WW3D2 loaders compile against - collision types, the chunk ids of
    # obsolete model formats, error codes, persistence ids - simid-closure.py's constants pass
    Core/Libraries/Source/WWVegas/WW3D2/coltype.h
    Core/Libraries/Source/WWVegas/WW3D2/w3d_obsolete.h
    Core/Libraries/Source/WWVegas/WW3D2/w3derr.h
    Core/Libraries/Source/WWVegas/WW3D2/ww3dids.h
    # det-codex r6/r7: the libraries under the digested code, whole. WWLib holds the chunk loader, the name lookups that
    # decide WHICH tree and animation the pose code finds, the strings, vectors, pools and file factory, and the stringex
    # and STLUtils helpers Common calls. WWSaveLoad registers the digested WWMath curves. Compression unpacks map chunks.
    # Dependencies/Utility and the string_compat files are the shims every digested file compiles against: stricmp,
    # _strlwr, the endian swaps, and itoa, which on 06/08/2026 was a cross-platform desync of its own. 17 + 3 + 2 + 5
    # commits in 07-10/2026: cheap. Found by a one-level closure of every whole-digested object - DWARF line rows for
    # inlined code, undefined symbols for calls. The rest of that closure is a named ceiling in SIMDIGEST_PATHS_REGION_*.
    Core/Libraries/Source/WWVegas/WWLib
    Core/Libraries/Source/WWVegas/WWSaveLoad
    Core/Libraries/Source/Compression
    Dependencies/Utility
    Generals/Code/CompatLib/Include/string_compat.h
    # det-codex-5: the rest of the language shims every digested file compiles against - TRUE, FALSE, ARRAY_SIZE, _isnan,
    # the tchar and wide-string aliases - simid-closure.py's constants pass
    Generals/Code/CompatLib/Include/types_compat.h
    Generals/Code/CompatLib/Include/tchar_compat.h
    Generals/Code/CompatLib/Include/wchar_compat.h
    # det-codex-5 Codex r28: __max / __min and the other Win32 shims digested code computes with
    Generals/Code/CompatLib/Include/windows_compat.h
    GeneralsMD/Code/CompatLib/Include/windows_compat.h
    GeneralsMD/Code/CompatLib/Include/string_compat.h
    GeneralsMD/Code/CompatLib/Source/string_compat.cpp
    # The Random-faction filter GameLogic applies at start, and the signature check that decides whether a signed
    # admin order runs - GXAdminOrder.h and GXKickTicket.h, already in Common, call it.
    Core/GameEngine/Source/GameClient/GUI/ChallengeGenerals.cpp
    Core/GameEngine/Include/GameClient/ChallengeGenerals.h
    Core/GameEngine/Source/GameClient/GXEd25519.cpp
    Core/GameEngine/Include/GameClient/GXEd25519.h
    Core/GameEngine/Source/GameClient/GXSha512.cpp
    Core/GameEngine/Include/GameClient/GXSha512.h
    # det-codex r7: client-folder math the simulation calls - ClipLine2D, PointInsideArea2D and friends from AIPathfind,
    # AIGroup, BridgeBehavior and PartitionManager, and NormalizeToRange/MuLaw from RailroadGuideAIUpdate.
    Core/GameEngine/Source/GameClient/Line2D.cpp
    Core/GameEngine/Include/GameClient/Line2D.h
    Core/GameEngine/Source/GameClient/Statistics.cpp
    Core/GameEngine/Include/GameClient/Statistics.h
    # det-codex-4: the client's last message translator decides which local messages reach the command list - network
    # orders, and the new-game / clear-data / frame-tick messages each peer makes for itself. The simulation reads them.
    Core/GameEngine/Source/GameClient/GameClientDispatch.cpp
    # det-codex simid-closure: small headers the closure tool found digested code inlining - the shroud-drawn switch
    # the ghost objects read, the client random-value macros ScriptEngine expands, the drawable's info block.
    Core/GameEngine/Include/GameClient/GXNoShroud.h
    Core/GameEngine/Include/GameClient/ClientRandomValue.h
    GeneralsMD/Code/GameEngine/Include/GameClient/DrawableInfo.h
    Generals/Code/GameEngine/Include/GameClient/DrawableInfo.h
    # det-codex r10: the BIG and local file systems - mount order, archive precedence, the BIG reader and the case-blind
    # local lookup decide WHICH W3D model bytes load, and W3D art is in no identity hash. Codex r10 refuted the old
    # ceiling with that path, and the history agrees: mount-precedence commits shipped as client-only.
    Core/GameEngineDevice/Include/StdDevice/Common/GXArchiveMount.h
    Core/GameEngineDevice/Include/StdDevice/Common/StdAssetMarker.h
    Core/GameEngineDevice/Include/StdDevice/Common/StdBIGFile.h
    Core/GameEngineDevice/Include/StdDevice/Common/StdBIGFileSystem.h
    Core/GameEngineDevice/Include/StdDevice/Common/StdLocalFile.h
    Core/GameEngineDevice/Include/StdDevice/Common/StdLocalFileSystem.h
    Core/GameEngineDevice/Include/Win32Device/Common/Win32BIGFile.h
    Core/GameEngineDevice/Include/Win32Device/Common/Win32BIGFileSystem.h
    Core/GameEngineDevice/Include/Win32Device/Common/Win32LocalFile.h
    Core/GameEngineDevice/Include/Win32Device/Common/Win32LocalFileSystem.h
    Core/GameEngineDevice/Source/StdDevice/Common/StdBIGFile.cpp
    Core/GameEngineDevice/Source/StdDevice/Common/StdBIGFileSystem.cpp
    Core/GameEngineDevice/Source/StdDevice/Common/StdLocalFile.cpp
    Core/GameEngineDevice/Source/StdDevice/Common/StdLocalFileSystem.cpp
    Core/GameEngineDevice/Source/Win32Device/Common/Win32BIGFile.cpp
    Core/GameEngineDevice/Source/Win32Device/Common/Win32BIGFileSystem.cpp
    Core/GameEngineDevice/Source/Win32Device/Common/Win32LocalFile.cpp
    Core/GameEngineDevice/Source/Win32Device/Common/Win32LocalFileSystem.cpp
    # det-codex r9: the W3DModelDraw subclasses. Bone validation can run on the DRAWN render object, and the regioned
    # setModelState calls their virtual onRenderObjRecreated - so their hooks shape the pose the simulation reads.
    Core/GameEngineDevice/Include/W3DDevice/GameClient/Module/W3DTankDraw.h
    Core/GameEngineDevice/Source/W3DDevice/GameClient/Drawable/Draw/W3DTankDraw.cpp
    Core/GameEngineDevice/Include/W3DDevice/GameClient/Module/W3DTruckDraw.h
    Core/GameEngineDevice/Source/W3DDevice/GameClient/Drawable/Draw/W3DTruckDraw.cpp
    Core/GameEngineDevice/Include/W3DDevice/GameClient/Module/W3DTankTruckDraw.h
    Core/GameEngineDevice/Source/W3DDevice/GameClient/Drawable/Draw/W3DTankTruckDraw.cpp
    Core/GameEngineDevice/Include/W3DDevice/GameClient/Module/W3DSupplyDraw.h
    Core/GameEngineDevice/Source/W3DDevice/GameClient/Drawable/Draw/W3DSupplyDraw.cpp
    Core/GameEngineDevice/Include/W3DDevice/GameClient/Module/W3DDependencyModelDraw.h
    Core/GameEngineDevice/Source/W3DDevice/GameClient/Drawable/Draw/W3DDependencyModelDraw.cpp
    Core/GameEngineDevice/Include/W3DDevice/GameClient/Module/W3DOverlordAircraftDraw.h
    Core/GameEngineDevice/Source/W3DDevice/GameClient/Drawable/Draw/W3DOverlordAircraftDraw.cpp
    Core/GameEngineDevice/Include/W3DDevice/GameClient/Module/W3DOverlordTankDraw.h
    Core/GameEngineDevice/Source/W3DDevice/GameClient/Drawable/Draw/W3DOverlordTankDraw.cpp
    Core/GameEngineDevice/Include/W3DDevice/GameClient/Module/W3DOverlordTruckDraw.h
    Core/GameEngineDevice/Source/W3DDevice/GameClient/Drawable/Draw/W3DOverlordTruckDraw.cpp
    Core/GameEngineDevice/Include/W3DDevice/GameClient/Module/W3DPoliceCarDraw.h
    Core/GameEngineDevice/Source/W3DDevice/GameClient/Drawable/Draw/W3DPoliceCarDraw.cpp
    Core/GameEngineDevice/Include/W3DDevice/GameClient/Module/W3DScienceModelDraw.h
    Core/GameEngineDevice/Source/W3DDevice/GameClient/Drawable/Draw/W3DScienceModelDraw.cpp
    # det-codex-3 Codex r16: the region HEADERS whole - their class declarations, every member the regioned code and
    # its whole-function constructors read; a member type changed outside the old regions moved no digest.
    Core/GameEngineDevice/Include/W3DDevice/GameClient/BaseHeightMap.h
    Core/GameEngine/Include/GameClient/ControlBar.h
    Core/GameEngine/Include/GameClient/MapUtil.h
)

# Build configuration is part of the simulation identity: a flag change can move
# code generation without moving a line of engine source.
set(SIMDIGEST_PATHS_BUILD
    cmake
    triplets
    CMakePresets.json
    vcpkg.json
    vcpkg-lock.json
    # GeneralsX @bugfix Claude 01/10/2026 The CMakeLists.txt on every path to the digested sources, Cd-31. They choose
    # WHICH files compile - Core/GameEngine/CMakeLists.txt comments out the Core copies the games replace - and set
    # definitions, so a source-list edit changed the simulation without moving the digest. A file pathspec matches that
    # file alone, and both digests take every entry: a Generals list edit moves ZH too, the safe direction.
    CMakeLists.txt
    Core/CMakeLists.txt
    Core/GameEngine/CMakeLists.txt
    Core/Libraries/CMakeLists.txt
    Core/Libraries/Source/WWVegas/CMakeLists.txt
    Core/Libraries/Source/WWVegas/WWMath/CMakeLists.txt
    GeneralsMD/CMakeLists.txt
    GeneralsMD/Code/CMakeLists.txt
    GeneralsMD/Code/GameEngine/CMakeLists.txt
    Generals/CMakeLists.txt
    Generals/Code/CMakeLists.txt
    Generals/Code/GameEngine/CMakeLists.txt
    # GeneralsX det-codex r2: the recipes of the WW3D2 and WWLib targets whose pose and loader code is digested.
    Core/Libraries/Source/WWVegas/WW3D2/CMakeLists.txt
    Core/Libraries/Source/WWVegas/WWLib/CMakeLists.txt
    GeneralsMD/Code/Libraries/CMakeLists.txt
    GeneralsMD/Code/Libraries/Source/WWVegas/CMakeLists.txt
    GeneralsMD/Code/Libraries/Source/WWVegas/WW3D2/CMakeLists.txt
    Generals/Code/Libraries/CMakeLists.txt
    Generals/Code/Libraries/Source/WWVegas/CMakeLists.txt
    Generals/Code/Libraries/Source/WWVegas/WW3D2/CMakeLists.txt
    # det-codex r6/r7: the recipes of the libraries digested whole above - CompatLib compiles string_compat.cpp.
    GeneralsMD/Code/CompatLib/CMakeLists.txt
    Core/Libraries/Source/WWVegas/WWSaveLoad/CMakeLists.txt
    Core/Libraries/Source/Compression/CMakeLists.txt
    Dependencies/Utility/CMakeLists.txt
)

set(SIMDIGEST_PATHS_CODE_ZH
    GeneralsMD/Code/GameEngine/Include/Common
    GeneralsMD/Code/GameEngine/Include/GameLogic
    GeneralsMD/Code/GameEngine/Include/GameNetwork
    GeneralsMD/Code/GameEngine/Source/Common
    GeneralsMD/Code/GameEngine/Source/GameLogic
    GeneralsMD/Code/GameEngine/Source/GameNetwork
    # GeneralsX @bugfix Claude 03/10/2026 Codex P2 det-codex-fixes: this game's sim-facing device files, see SHARED.
    GeneralsMD/Code/GameEngineDevice/Source/W3DDevice/GameLogic/W3DTerrainLogic.cpp
    GeneralsMD/Code/GameEngineDevice/Include/W3DDevice/GameLogic/W3DTerrainLogic.h
    GeneralsMD/Code/GameEngineDevice/Source/W3DDevice/GameLogic/W3DGameLogic.cpp
    GeneralsMD/Code/GameEngineDevice/Include/W3DDevice/GameLogic/W3DGameLogic.h
    GeneralsMD/Code/GameEngineDevice/Source/W3DDevice/Common/Thing/W3DModuleFactory.cpp
    GeneralsMD/Code/GameEngineDevice/Include/W3DDevice/Common/W3DModuleFactory.h
    GeneralsMD/Code/GameEngineDevice/Include/W3DDevice/GameClient/W3DBridgeBuffer.h
    GeneralsMD/Code/GameEngineDevice/Source/W3DDevice/GameClient/W3DAssetManager.cpp
    GeneralsMD/Code/GameEngineDevice/Include/W3DDevice/GameClient/W3DAssetManager.h
    GeneralsMD/Code/Libraries/Source/WWVegas/WW3D2/hlod.cpp
    GeneralsMD/Code/Libraries/Source/WWVegas/WW3D2/hlod.h
    GeneralsMD/Code/Libraries/Source/WWVegas/WW3D2/animobj.cpp
    GeneralsMD/Code/Libraries/Source/WWVegas/WW3D2/animobj.h
    GeneralsMD/Code/Libraries/Source/WWVegas/WW3D2/assetmgr.cpp
    GeneralsMD/Code/Libraries/Source/WWVegas/WW3D2/assetmgr.h
    GeneralsMD/Code/Libraries/Source/WWVegas/WW3D2/hanimmgr.cpp
    GeneralsMD/Code/Libraries/Source/WWVegas/WW3D2/hanimmgr.h
    GeneralsMD/Code/Libraries/Source/WWVegas/WW3D2/hrawanim.cpp
    GeneralsMD/Code/Libraries/Source/WWVegas/WW3D2/hrawanim.h
    GeneralsMD/Code/Libraries/Source/WWVegas/WW3D2/motchan.cpp
    GeneralsMD/Code/Libraries/Source/WWVegas/WW3D2/motchan.h
    GeneralsMD/Code/Libraries/Source/WWVegas/WW3D2/htreemgr.cpp
    GeneralsMD/Code/Libraries/Source/WWVegas/WW3D2/htreemgr.h
    GeneralsMD/Code/GameEngineDevice/Source/W3DDevice/GameClient/W3DFileSystem.cpp
    GeneralsMD/Code/GameEngineDevice/Include/W3DDevice/GameClient/W3DFileSystem.h
    GeneralsMD/Code/Libraries/Source/WWVegas/WW3D2/mesh.cpp
    GeneralsMD/Code/Libraries/Source/WWVegas/WW3D2/mesh.h
    GeneralsMD/Code/Libraries/Source/WWVegas/WW3D2/meshmdlio.cpp
    GeneralsMD/Code/Libraries/Source/WWVegas/WW3D2/meshmdl.h
    GeneralsMD/Code/Libraries/Source/WWVegas/WW3D2/w3d_file.h
    GeneralsMD/Code/Libraries/Source/WWVegas/WW3D2/meshgeometry.cpp
    GeneralsMD/Code/Libraries/Source/WWVegas/WW3D2/meshgeometry.h
    GeneralsMD/Code/Libraries/Source/WWVegas/WW3D2/hmorphanim.cpp
    GeneralsMD/Code/Libraries/Source/WWVegas/WW3D2/hmorphanim.h
    # det-codex r7: the mesh model meshmdlio fills and mesh.cpp makes unique - the vertices bridges are built from -
    # and the WW3D sync clock HTreeClass::Anim_Update_Without_Interpolation branches on.
    GeneralsMD/Code/Libraries/Source/WWVegas/WW3D2/meshmdl.cpp
    GeneralsMD/Code/Libraries/Source/WWVegas/WW3D2/ww3d.h
    # det-codex r9: the device implementations of logic interfaces the simulation calls - the lexicon whose names are
    # interned before UpgradeCenter - item 4c - the ghost objects canTransferSuppliesAt reads, the thing factory.
    GeneralsMD/Code/GameEngineDevice/Include/W3DDevice/Common/W3DFunctionLexicon.h
    GeneralsMD/Code/GameEngineDevice/Include/W3DDevice/Common/W3DThingFactory.h
    GeneralsMD/Code/GameEngineDevice/Include/W3DDevice/GameLogic/W3DGhostObject.h
    GeneralsMD/Code/GameEngineDevice/Source/W3DDevice/Common/System/W3DFunctionLexicon.cpp
    GeneralsMD/Code/GameEngineDevice/Source/W3DDevice/Common/Thing/W3DThingFactory.cpp
    GeneralsMD/Code/GameEngineDevice/Source/W3DDevice/GameLogic/W3DGhostObject.cpp
    # det-codex-3: the device GameClient whole - its factories pick the terrain visual, the in-game UI that owns the
    # command dictionaries and the display that owns the asset manager, and its init, reset, update and time of
    # day forward to the regioned GameClient. Two commits since 07/2026, both files.
    GeneralsMD/Code/GameEngineDevice/Source/W3DDevice/GameClient/W3DGameClient.cpp
    GeneralsMD/Code/GameEngineDevice/Include/W3DDevice/GameClient/W3DGameClient.h
    # det-codex r8: this game's copies of the model-content render-object classes, see SHARED.
    GeneralsMD/Code/Libraries/Source/WWVegas/WW3D2/nullrobj.cpp
    GeneralsMD/Code/Libraries/Source/WWVegas/WW3D2/nullrobj.h
    GeneralsMD/Code/Libraries/Source/WWVegas/WW3D2/boxrobj.cpp
    GeneralsMD/Code/Libraries/Source/WWVegas/WW3D2/boxrobj.h
    GeneralsMD/Code/Libraries/Source/WWVegas/WW3D2/dazzle.cpp
    GeneralsMD/Code/Libraries/Source/WWVegas/WW3D2/dazzle.h
    GeneralsMD/Code/Libraries/Source/WWVegas/WW3D2/part_emt.cpp
    GeneralsMD/Code/Libraries/Source/WWVegas/WW3D2/part_emt.h
    GeneralsMD/Code/Libraries/Source/WWVegas/WW3D2/part_ldr.cpp
    GeneralsMD/Code/Libraries/Source/WWVegas/WW3D2/part_ldr.h
    # det-codex-3 Codex r16: the region HEADERS whole - their class declarations, every member the regioned code and
    # its whole-function constructors read; a member type changed outside the old regions moved no digest.
    GeneralsMD/Code/GameEngine/Include/GameClient/Drawable.h
    GeneralsMD/Code/GameEngine/Include/GameClient/GameClient.h
    # det-codex-5: the precompiled header every digested GameEngine file starts with, and the shell-hook ids Scripts.cpp
    # names in order - simid-closure.py's constants pass
    GeneralsMD/Code/GameEngine/Include/Precompiled/PreRTS.h
    GeneralsMD/Code/GameEngine/Include/GameClient/ShellHooks.h
    GeneralsMD/Code/GameEngineDevice/Include/Win32Device/Common/Win32GameEngine.h
)

set(SIMDIGEST_PATHS_CODE_G
    Generals/Code/GameEngine/Include/Common
    Generals/Code/GameEngine/Include/GameLogic
    Generals/Code/GameEngine/Include/GameNetwork
    Generals/Code/GameEngine/Source/Common
    Generals/Code/GameEngine/Source/GameLogic
    Generals/Code/GameEngine/Source/GameNetwork
    # GeneralsX @bugfix Claude 03/10/2026 Codex P2 det-codex-fixes: this game's sim-facing device files, see SHARED.
    Generals/Code/GameEngineDevice/Source/W3DDevice/GameLogic/W3DTerrainLogic.cpp
    Generals/Code/GameEngineDevice/Include/W3DDevice/GameLogic/W3DTerrainLogic.h
    Generals/Code/GameEngineDevice/Source/W3DDevice/GameLogic/W3DGameLogic.cpp
    Generals/Code/GameEngineDevice/Include/W3DDevice/GameLogic/W3DGameLogic.h
    Generals/Code/GameEngineDevice/Source/W3DDevice/Common/Thing/W3DModuleFactory.cpp
    Generals/Code/GameEngineDevice/Include/W3DDevice/Common/W3DModuleFactory.h
    Generals/Code/GameEngineDevice/Include/W3DDevice/GameClient/W3DBridgeBuffer.h
    Generals/Code/GameEngineDevice/Source/W3DDevice/GameClient/W3DAssetManager.cpp
    Generals/Code/GameEngineDevice/Include/W3DDevice/GameClient/W3DAssetManager.h
    Generals/Code/Libraries/Source/WWVegas/WW3D2/hlod.cpp
    Generals/Code/Libraries/Source/WWVegas/WW3D2/hlod.h
    Generals/Code/Libraries/Source/WWVegas/WW3D2/animobj.cpp
    Generals/Code/Libraries/Source/WWVegas/WW3D2/animobj.h
    Generals/Code/Libraries/Source/WWVegas/WW3D2/assetmgr.cpp
    Generals/Code/Libraries/Source/WWVegas/WW3D2/assetmgr.h
    Generals/Code/Libraries/Source/WWVegas/WW3D2/hanimmgr.cpp
    Generals/Code/Libraries/Source/WWVegas/WW3D2/hanimmgr.h
    Generals/Code/Libraries/Source/WWVegas/WW3D2/hrawanim.cpp
    Generals/Code/Libraries/Source/WWVegas/WW3D2/hrawanim.h
    Generals/Code/Libraries/Source/WWVegas/WW3D2/motchan.cpp
    Generals/Code/Libraries/Source/WWVegas/WW3D2/motchan.h
    Generals/Code/Libraries/Source/WWVegas/WW3D2/htreemgr.cpp
    Generals/Code/Libraries/Source/WWVegas/WW3D2/htreemgr.h
    Generals/Code/GameEngineDevice/Source/W3DDevice/GameClient/W3DFileSystem.cpp
    Generals/Code/GameEngineDevice/Include/W3DDevice/GameClient/W3DFileSystem.h
    Generals/Code/Libraries/Source/WWVegas/WW3D2/mesh.cpp
    Generals/Code/Libraries/Source/WWVegas/WW3D2/mesh.h
    Generals/Code/Libraries/Source/WWVegas/WW3D2/meshmdlio.cpp
    Generals/Code/Libraries/Source/WWVegas/WW3D2/meshmdl.h
    Generals/Code/Libraries/Source/WWVegas/WW3D2/w3d_file.h
    Generals/Code/Libraries/Source/WWVegas/WW3D2/meshgeometry.cpp
    Generals/Code/Libraries/Source/WWVegas/WW3D2/meshgeometry.h
    Generals/Code/Libraries/Source/WWVegas/WW3D2/hmorphanim.cpp
    Generals/Code/Libraries/Source/WWVegas/WW3D2/hmorphanim.h
    # det-codex r7: the mesh model meshmdlio fills and mesh.cpp makes unique - the vertices bridges are built from -
    # and the WW3D sync clock HTreeClass::Anim_Update_Without_Interpolation branches on.
    Generals/Code/Libraries/Source/WWVegas/WW3D2/meshmdl.cpp
    Generals/Code/Libraries/Source/WWVegas/WW3D2/ww3d.h
    # det-codex r9: the device implementations of logic interfaces the simulation calls - the lexicon whose names are
    # interned before UpgradeCenter - item 4c - the ghost objects canTransferSuppliesAt reads, the thing factory.
    Generals/Code/GameEngineDevice/Include/W3DDevice/Common/W3DFunctionLexicon.h
    Generals/Code/GameEngineDevice/Include/W3DDevice/Common/W3DThingFactory.h
    Generals/Code/GameEngineDevice/Include/W3DDevice/GameLogic/W3DGhostObject.h
    Generals/Code/GameEngineDevice/Source/W3DDevice/Common/System/W3DFunctionLexicon.cpp
    Generals/Code/GameEngineDevice/Source/W3DDevice/Common/Thing/W3DThingFactory.cpp
    Generals/Code/GameEngineDevice/Source/W3DDevice/GameLogic/W3DGhostObject.cpp
    # det-codex-3: the device GameClient whole - its factories pick the terrain visual, the in-game UI that owns the
    # command dictionaries and the display that owns the asset manager, and its init, reset, update and time of
    # day forward to the regioned GameClient. Two commits since 07/2026, both files.
    Generals/Code/GameEngineDevice/Source/W3DDevice/GameClient/W3DGameClient.cpp
    Generals/Code/GameEngineDevice/Include/W3DDevice/GameClient/W3DGameClient.h
    # det-codex r8: this game's copies of the model-content render-object classes, see SHARED.
    Generals/Code/Libraries/Source/WWVegas/WW3D2/nullrobj.cpp
    Generals/Code/Libraries/Source/WWVegas/WW3D2/nullrobj.h
    Generals/Code/Libraries/Source/WWVegas/WW3D2/boxrobj.cpp
    Generals/Code/Libraries/Source/WWVegas/WW3D2/boxrobj.h
    Generals/Code/Libraries/Source/WWVegas/WW3D2/dazzle.cpp
    Generals/Code/Libraries/Source/WWVegas/WW3D2/dazzle.h
    Generals/Code/Libraries/Source/WWVegas/WW3D2/part_emt.cpp
    Generals/Code/Libraries/Source/WWVegas/WW3D2/part_emt.h
    Generals/Code/Libraries/Source/WWVegas/WW3D2/part_ldr.cpp
    Generals/Code/Libraries/Source/WWVegas/WW3D2/part_ldr.h
    # det-codex-3 Codex r16: the region HEADERS whole - their class declarations, every member the regioned code and
    # its whole-function constructors read; a member type changed outside the old regions moved no digest.
    Generals/Code/GameEngine/Include/GameClient/Drawable.h
    Generals/Code/GameEngine/Include/GameClient/GameClient.h
    # det-codex-5: the precompiled header every digested GameEngine file starts with, and the shell-hook ids Scripts.cpp
    # names in order - simid-closure.py's constants pass
    Generals/Code/GameEngine/Include/Precompiled/PreRTS.h
    Generals/Code/GameEngine/Include/GameClient/ShellHooks.h
    Generals/Code/GameEngineDevice/Include/Win32Device/Common/Win32GameEngine.h
)

# GeneralsX @bugfix Claude 03/10/2026 Codex P2 det-codex-fixes. HOT render files with a few functions the simulation
# reads. Only the text from a line containing GX-SIMID-BEGIN up to the next line containing GX-SIMID-END is hashed, so a
# rendering edit elsewhere in the file stays client-only (whole, these files would have turned 189 client-only commits
# of 07-10/2026 into four-platform ships). A region is a comment pair around whole functions - mark every function the
# simulation calls AND the file-local helpers they call. Fails closed: a listed file without a region, an unbalanced
# or nested marker, or a marker in a code file no list names stops the build.
# Not digested on purpose: audio length and music-finished - only the SPEECH/AUDIO/MUSIC script conditions read them,
# and Determinism-1 classifies those client-local; and the GameEngineDevice CMakeLists.txt, which add render files
# weekly and set no codegen flag - a swap of WHICH copy of a listed file compiles there needs a list edit here anyway.
# THE BOUNDARY IS A CHECKED RULE (det-codex, lead C11 03/10/2026): the digested code must be CLOSED - everything it
# reaches (inlined code, calls and globals, pointers in its tables, overrides of its virtuals) is digested here, or
# named in resources/gitinfo/simdigest_exclusions.txt with the reason the simulation does not read it. Check it with
#   scripts/build/simid-closure.py <a -g build dir>
# after changing these lists or adding code the simulation calls; a GAP it prints is a file to digest or an entry to
# justify. It reads a build's objects, so it is run by hand before a promote, not by this script.
set(SIMDIGEST_PATHS_REGION_SHARED
    Core/GameEngineDevice/Source/W3DDevice/GameClient/BaseHeightMap.cpp
    Core/GameEngineDevice/Source/W3DDevice/GameClient/HeightMap.cpp
    Core/GameEngineDevice/Source/W3DDevice/GameClient/W3DTerrainVisual.cpp
    Core/GameEngineDevice/Source/W3DDevice/GameClient/Water/W3DWater.cpp
    Core/GameEngineDevice/Source/W3DDevice/GameClient/Drawable/Draw/W3DModelDraw.cpp
    Core/GameEngineDevice/Source/W3DDevice/GameClient/Drawable/Draw/W3DLaserDraw.cpp
    Core/GameEngine/Source/GameClient/GUI/ControlBar/ControlBar.cpp
    Core/GameEngine/Source/GameClient/MapUtil.cpp
)
set(SIMDIGEST_PATHS_REGION_ZH
    GeneralsMD/Code/GameEngine/Source/GameClient/Drawable.cpp
    GeneralsMD/Code/GameEngineDevice/Source/W3DDevice/GameClient/W3DDisplay.cpp
    GeneralsMD/Code/GameEngineDevice/Source/W3DDevice/GameClient/W3DBridgeBuffer.cpp
    GeneralsMD/Code/Libraries/Source/WWVegas/WW3D2/ww3d.cpp
    GeneralsMD/Code/GameEngineDevice/Source/SDL3GameEngine.cpp
    GeneralsMD/Code/GameEngine/Source/GameClient/GameClient.cpp
    GeneralsMD/Code/GameEngine/Source/GameClient/InGameUI.cpp
    GeneralsMD/Code/GameEngineDevice/Source/W3DDevice/GameClient/W3DInGameUI.cpp
)
set(SIMDIGEST_PATHS_REGION_G
    Generals/Code/GameEngine/Source/GameClient/Drawable.cpp
    Generals/Code/GameEngineDevice/Source/W3DDevice/GameClient/W3DDisplay.cpp
    Generals/Code/GameEngineDevice/Source/W3DDevice/GameClient/W3DBridgeBuffer.cpp
    Generals/Code/Libraries/Source/WWVegas/WW3D2/ww3d.cpp
    Generals/Code/GameEngineDevice/Source/SDL3GameEngine.cpp
    Generals/Code/GameEngine/Source/GameClient/GameClient.cpp
    Generals/Code/GameEngine/Source/GameClient/InGameUI.cpp
    Generals/Code/GameEngineDevice/Source/W3DDevice/GameClient/W3DInGameUI.cpp
)

# GeneralsX @bugfix Claude 03/10/2026 Codex P2 det-codex-fixes, the guard lead C11 asked for: what each region file's
# markers MUST hold - the simulation-facing definitions found by the 02-03/10 call-graph search. "<file>|<Class::fn>" is a
# definition, every overload; "<file>|=<text>" a literal, no semicolon - CMake would split the entry there. Checked by
# SimDigestCheckRegionFunctions on every build.
set(SIMDIGEST_REGION_FUNCTIONS
    # det-codex-5 Codex r22: the constants region code uses (#1), the control bar's lifecycle in the in-game UI and the
    # forwarders that reach it (#2), the map-path resolver, the scan's hand-off of the path and the two statements that record
    # it (#3); and the in-game UI's and the map cache's globals, as r17's rule asks
    "Core/GameEngineDevice/Source/W3DDevice/GameClient/Drawable/Draw/W3DModelDraw.cpp|=enum INIReadFlagsType"
    "Core/GameEngineDevice/Source/W3DDevice/GameClient/Drawable/Draw/W3DModelDraw.cpp|=enum ParseCondStateType"
    "Core/GameEngineDevice/Source/W3DDevice/GameClient/Drawable/Draw/W3DModelDraw.cpp|=enum AnimParseType"
    "Core/GameEngineDevice/Source/W3DDevice/GameClient/Drawable/Draw/W3DModelDraw.cpp|=NO_NEXT_DURATION = 0xffffffff"
    "Core/GameEngineDevice/Source/W3DDevice/GameClient/HeightMap.cpp|=#define DEFAULT_MAX_MAP_EXTRABLEND_TILES"
    "GeneralsMD/Code/GameEngine/Source/GameClient/GameClient.cpp|=#define DRAWABLE_HASH_SIZE"
    "Generals/Code/GameEngine/Source/GameClient/GameClient.cpp|=#define DRAWABLE_HASH_SIZE"
    "GeneralsMD/Code/GameEngine/Source/GameClient/InGameUI.cpp|=TheControlBar = NEW ControlBar"
    "GeneralsMD/Code/GameEngine/Source/GameClient/InGameUI.cpp|=TheControlBar->init()"
    "GeneralsMD/Code/GameEngine/Source/GameClient/InGameUI.cpp|=TheControlBar->reset()"
    "GeneralsMD/Code/GameEngine/Source/GameClient/InGameUI.cpp|=delete TheControlBar"
    "Generals/Code/GameEngine/Source/GameClient/InGameUI.cpp|=TheControlBar = NEW ControlBar"
    "Generals/Code/GameEngine/Source/GameClient/InGameUI.cpp|=TheControlBar->init()"
    "Generals/Code/GameEngine/Source/GameClient/InGameUI.cpp|=TheControlBar->reset()"
    "Generals/Code/GameEngine/Source/GameClient/InGameUI.cpp|=delete TheControlBar"
    "GeneralsMD/Code/GameEngineDevice/Source/W3DDevice/GameClient/W3DInGameUI.cpp|W3DInGameUI::init"
    "GeneralsMD/Code/GameEngineDevice/Source/W3DDevice/GameClient/W3DInGameUI.cpp|W3DInGameUI::reset"
    "Generals/Code/GameEngineDevice/Source/W3DDevice/GameClient/W3DInGameUI.cpp|W3DInGameUI::init"
    "Generals/Code/GameEngineDevice/Source/W3DDevice/GameClient/W3DInGameUI.cpp|W3DInGameUI::reset"
    "Core/GameEngine/Source/GameClient/MapUtil.cpp|MapCache::getPathOnDisk"
    "Core/GameEngine/Source/GameClient/MapUtil.cpp|=m_filePathOnDisk = fname"
    # det-codex-5 Codex r23: a cache miss publishes the entry that carries the path
    "Core/GameEngine/Source/GameClient/MapUtil.cpp|=(*this)[lowerFname] = md"
    "Core/GameEngine/Source/GameClient/MapUtil.cpp|=addMap(mapDir, *filepathIt, filepathLower, fileInfo, isOfficial)"
    "Core/GameEngine/Source/GameClient/MapUtil.cpp|=MapCache *TheMapCache = nullptr"
    "GeneralsMD/Code/GameEngine/Source/GameClient/InGameUI.cpp|=InGameUI *TheInGameUI = nullptr"
    "Generals/Code/GameEngine/Source/GameClient/InGameUI.cpp|=InGameUI *TheInGameUI = nullptr"
    # det-codex-3 Codex r17: the definitions of the globals and static members region code reads
    "Core/GameEngine/Source/GameClient/GUI/ControlBar/ControlBar.cpp|=ControlBar *TheControlBar = nullptr"
    "Core/GameEngineDevice/Source/W3DDevice/GameClient/BaseHeightMap.cpp|=BaseHeightMapRenderObjClass *TheTerrainRenderObject=nullptr"
    "Core/GameEngineDevice/Source/W3DDevice/GameClient/HeightMap.cpp|=HeightMapRenderObjClass *TheHeightMap = nullptr"
    "Core/GameEngineDevice/Source/W3DDevice/GameClient/Water/W3DWater.cpp|=WaterRenderObjClass *TheWaterRenderObj=nullptr"
    "GeneralsMD/Code/GameEngine/Source/GameClient/GameClient.cpp|=GameClient *TheGameClient = nullptr"
    "GeneralsMD/Code/GameEngine/Source/GameClient/GameClient.cpp|=UnsignedInt GameClient::s_drawableChangeCount = 0"
    "GeneralsMD/Code/GameEngine/Source/GameClient/Drawable.cpp|=Drawable::s_modelLockCount = 0"
    "GeneralsMD/Code/GameEngine/Source/GameClient/Drawable.cpp|=Drawable::s_anyModelDirty = TRUE"
    "GeneralsMD/Code/GameEngineDevice/Source/W3DDevice/GameClient/W3DDisplay.cpp|=W3DAssetManager *W3DDisplay::m_assetManager = nullptr"
    "GeneralsMD/Code/Libraries/Source/WWVegas/WW3D2/ww3d.cpp|=WW3D::MungeSortOnLoad = false"
    "Generals/Code/GameEngine/Source/GameClient/GameClient.cpp|=GameClient *TheGameClient = nullptr"
    "Generals/Code/GameEngine/Source/GameClient/GameClient.cpp|=UnsignedInt GameClient::s_drawableChangeCount = 0"
    "Generals/Code/GameEngine/Source/GameClient/Drawable.cpp|=Drawable::s_modelLockCount = 0"
    "Generals/Code/GameEngine/Source/GameClient/Drawable.cpp|=Drawable::s_anyModelDirty = TRUE"
    "Generals/Code/GameEngineDevice/Source/W3DDevice/GameClient/W3DDisplay.cpp|=W3DAssetManager *W3DDisplay::m_assetManager = nullptr"
    "Generals/Code/Libraries/Source/WWVegas/WW3D2/ww3d.cpp|=WW3D::MungeSortOnLoad = false"
    # det-codex-3 (r15 open list): the client lifecycle that creates, resets and steps what the simulation reads - the
    # display (asset manager), in-game UI (command dictionaries), challenge filter and terrain visual; the drawable
    # teardown and list root; the ghost-object and shroud-status refresh; the terrain visual update (water grid); time
    # of day (the NIGHT model condition); the map binding, reset and LOD swap of the terrain render object; the bridge
    # model check; the removal of map-overridden command sets and buttons; the factory that picks the game client.
    "Core/GameEngineDevice/Source/W3DDevice/GameClient/W3DTerrainVisual.cpp|W3DTerrainVisual::W3DTerrainVisual"
    "Core/GameEngineDevice/Source/W3DDevice/GameClient/W3DTerrainVisual.cpp|W3DTerrainVisual::init"
    "Core/GameEngineDevice/Source/W3DDevice/GameClient/W3DTerrainVisual.cpp|W3DTerrainVisual::reset"
    "Core/GameEngineDevice/Source/W3DDevice/GameClient/W3DTerrainVisual.cpp|W3DTerrainVisual::canLoadEveryBridgeModel"
    "Core/GameEngineDevice/Source/W3DDevice/GameClient/BaseHeightMap.cpp|BaseHeightMapRenderObjClass::reset"
    "Core/GameEngineDevice/Source/W3DDevice/GameClient/BaseHeightMap.cpp|BaseHeightMapRenderObjClass::freeMapResources"
    "Core/GameEngineDevice/Source/W3DDevice/GameClient/BaseHeightMap.cpp|BaseHeightMapRenderObjClass::adjustTerrainLOD"
    "Core/GameEngineDevice/Source/W3DDevice/GameClient/HeightMap.cpp|HeightMapRenderObjClass::reset"
    "Core/GameEngineDevice/Source/W3DDevice/GameClient/HeightMap.cpp|HeightMapRenderObjClass::freeMapResources"
    "Core/GameEngineDevice/Source/W3DDevice/GameClient/HeightMap.cpp|HeightMapRenderObjClass::adjustTerrainLOD"
    "Core/GameEngine/Source/GameClient/GUI/ControlBar/ControlBar.cpp|=Overridable *stillValid = set->deleteOverrides()"
    "Core/GameEngine/Source/GameClient/GUI/ControlBar/ControlBar.cpp|=Overridable *stillValid = button->deleteOverrides()"
    # det-codex-4 Codex r21: the constructor's command-dictionary roots, which CommandSet/CommandButton linking and the
    # findCommandSet walk (AIPlayer's upgrade decisions) start from
    "Core/GameEngine/Source/GameClient/GUI/ControlBar/ControlBar.cpp|=m_commandButtons = nullptr"
    "Core/GameEngine/Source/GameClient/GUI/ControlBar/ControlBar.cpp|=m_commandSets = nullptr"
    "GeneralsMD/Code/GameEngine/Source/GameClient/GameClient.cpp|GameClient::GameClient"
    "GeneralsMD/Code/GameEngine/Source/GameClient/GameClient.cpp|GameClient::reset"
    "GeneralsMD/Code/GameEngine/Source/GameClient/GameClient.cpp|GameClient::setTimeOfDay"
    "GeneralsMD/Code/GameEngine/Source/GameClient/GameClient.cpp|=TheDisplay = createGameDisplay()"
    "GeneralsMD/Code/GameEngine/Source/GameClient/GameClient.cpp|=TheInGameUI = createInGameUI()"
    "GeneralsMD/Code/GameEngine/Source/GameClient/GameClient.cpp|=TheChallengeGenerals = createChallengeGenerals()"
    "GeneralsMD/Code/GameEngine/Source/GameClient/GameClient.cpp|=TheTerrainVisual = createTerrainVisual()"
    "GeneralsMD/Code/GameEngine/Source/GameClient/GameClient.cpp|=TheGhostObjectManager->updateOrphanedObjects(nullptr, 0)"
    "GeneralsMD/Code/GameEngine/Source/GameClient/GameClient.cpp|=draw->setFullyObscuredByShroud(ss >= OBJECTSHROUD_FOGGED)"
    "GeneralsMD/Code/GameEngine/Source/GameClient/GameClient.cpp|=TheTerrainVisual->UPDATE()"
    "GeneralsMD/Code/GameEngineDevice/Source/SDL3GameEngine.cpp|SDL3GameEngine::createGameClient"
    "Generals/Code/GameEngine/Source/GameClient/GameClient.cpp|GameClient::GameClient"
    "Generals/Code/GameEngine/Source/GameClient/GameClient.cpp|GameClient::reset"
    "Generals/Code/GameEngine/Source/GameClient/GameClient.cpp|GameClient::setTimeOfDay"
    "Generals/Code/GameEngine/Source/GameClient/GameClient.cpp|=TheDisplay = createGameDisplay()"
    "Generals/Code/GameEngine/Source/GameClient/GameClient.cpp|=TheInGameUI = createInGameUI()"
    "Generals/Code/GameEngine/Source/GameClient/GameClient.cpp|=TheTerrainVisual = createTerrainVisual()"
    "Generals/Code/GameEngine/Source/GameClient/GameClient.cpp|=TheGhostObjectManager->updateOrphanedObjects(nullptr, 0)"
    "Generals/Code/GameEngine/Source/GameClient/GameClient.cpp|=draw->setFullyObscuredByShroud(ss >= OBJECTSHROUD_FOGGED)"
    "Generals/Code/GameEngine/Source/GameClient/GameClient.cpp|=TheTerrainVisual->UPDATE()"
    "Generals/Code/GameEngineDevice/Source/SDL3GameEngine.cpp|SDL3GameEngine::createGameClient"
    # det-codex r12: same-file helpers the regioned code calls, found once the tool checked calls within an object.
    "Core/GameEngine/Source/GameClient/GUI/ControlBar/ControlBar.cpp|CommandSet::friend_addToList"
    "Core/GameEngineDevice/Source/W3DDevice/GameClient/Drawable/Draw/W3DModelDraw.cpp|turretNamesDiffer"
    "GeneralsMD/Code/GameEngineDevice/Source/W3DDevice/GameClient/W3DBridgeBuffer.cpp|W3DBridge::W3DBridge"
    "GeneralsMD/Code/GameEngineDevice/Source/W3DDevice/GameClient/W3DBridgeBuffer.cpp|W3DBridge::~W3DBridge"
    "GeneralsMD/Code/GameEngineDevice/Source/W3DDevice/GameClient/W3DBridgeBuffer.cpp|W3DBridge::clearBridge"
    "Generals/Code/GameEngineDevice/Source/W3DDevice/GameClient/W3DBridgeBuffer.cpp|W3DBridge::W3DBridge"
    "Generals/Code/GameEngineDevice/Source/W3DDevice/GameClient/W3DBridgeBuffer.cpp|W3DBridge::~W3DBridge"
    "Generals/Code/GameEngineDevice/Source/W3DDevice/GameClient/W3DBridgeBuffer.cpp|W3DBridge::clearBridge"
    # det-codex r13: the drawable's binding to its object and its list links - getDrawableGeometryInfo reads the object,
    # the dirty-state flush walks the list.
    "GeneralsMD/Code/GameEngine/Source/GameClient/Drawable.cpp|Drawable::friend_bindToObject"
    "GeneralsMD/Code/GameEngine/Source/GameClient/Drawable.cpp|Drawable::prependToList"
    "Generals/Code/GameEngine/Source/GameClient/Drawable.cpp|Drawable::friend_bindToObject"
    "Generals/Code/GameEngine/Source/GameClient/Drawable.cpp|Drawable::prependToList"
    "Core/GameEngineDevice/Source/W3DDevice/GameClient/HeightMap.cpp|HeightMapRenderObjClass::HeightMapRenderObjClass"
    "Core/GameEngineDevice/Source/W3DDevice/GameClient/Water/W3DWater.cpp|WaterRenderObjClass::init"
    # det-codex r14: destroyDrawable (lookup table and binding), Drawable's Thing downcast (Module::m_drawable feeds
    # the barrel count), the drawable list roots.
    "GeneralsMD/Code/GameEngine/Source/GameClient/GameClient.cpp|GameClient::destroyDrawable"
    "GeneralsMD/Code/GameEngine/Source/GameClient/Drawable.cpp|Drawable::removeFromList"
    "Generals/Code/GameEngine/Source/GameClient/Drawable.cpp|Drawable::removeFromList"
    "Generals/Code/GameEngine/Source/GameClient/GameClient.cpp|GameClient::destroyDrawable"
    "Core/GameEngineDevice/Source/W3DDevice/GameClient/Drawable/Draw/W3DModelDraw.cpp|W3DModelDrawModuleData::getAttachToDrawableBoneOffset"
    "Core/GameEngineDevice/Source/W3DDevice/GameClient/Drawable/Draw/W3DModelDraw.cpp|W3DAnimationInfo::W3DAnimationInfo"
    "Core/GameEngineDevice/Source/W3DDevice/GameClient/Drawable/Draw/W3DModelDraw.cpp|W3DAnimationInfo::operator="
    "Core/GameEngineDevice/Source/W3DDevice/GameClient/HeightMap.cpp|HeightMapRenderObjClass::initHeightData"
    "Core/GameEngineDevice/Source/W3DDevice/GameClient/BaseHeightMap.cpp|BaseHeightMapRenderObjClass::initHeightData"
    "GeneralsMD/Code/GameEngineDevice/Source/W3DDevice/GameClient/W3DBridgeBuffer.cpp|W3DBridgeBuffer::W3DBridgeBuffer"
    "GeneralsMD/Code/GameEngineDevice/Source/W3DDevice/GameClient/W3DBridgeBuffer.cpp|W3DBridgeBuffer::clearAllBridges"
    "GeneralsMD/Code/GameEngineDevice/Source/W3DDevice/GameClient/W3DBridgeBuffer.cpp|=#define BRIDGE_FLOAT_AMT (0.25f)"
    "Generals/Code/GameEngineDevice/Source/W3DDevice/GameClient/W3DBridgeBuffer.cpp|W3DBridgeBuffer::W3DBridgeBuffer"
    "Generals/Code/GameEngineDevice/Source/W3DDevice/GameClient/W3DBridgeBuffer.cpp|W3DBridgeBuffer::clearAllBridges"
    "Generals/Code/GameEngineDevice/Source/W3DDevice/GameClient/W3DBridgeBuffer.cpp|=#define BRIDGE_FLOAT_AMT (0.25f)"
    "GeneralsMD/Code/GameEngine/Source/GameClient/Drawable.cpp|Drawable::Drawable"
    "GeneralsMD/Code/GameEngineDevice/Source/SDL3GameEngine.cpp|SDL3GameEngine::createGameLogic"
    "GeneralsMD/Code/GameEngineDevice/Source/SDL3GameEngine.cpp|SDL3GameEngine::createModuleFactory"
    "GeneralsMD/Code/GameEngineDevice/Source/SDL3GameEngine.cpp|SDL3GameEngine::createThingFactory"
    "GeneralsMD/Code/GameEngineDevice/Source/SDL3GameEngine.cpp|SDL3GameEngine::createFunctionLexicon"
    "GeneralsMD/Code/GameEngineDevice/Source/SDL3GameEngine.cpp|SDL3GameEngine::createLocalFileSystem"
    "GeneralsMD/Code/GameEngineDevice/Source/SDL3GameEngine.cpp|SDL3GameEngine::createArchiveFileSystem"
    "GeneralsMD/Code/Libraries/Source/WWVegas/WW3D2/ww3d.cpp|WW3D::Sync"
    "GeneralsMD/Code/Libraries/Source/WWVegas/WW3D2/ww3d.cpp|WW3D::Update_Logic_Frame_Time"
    "GeneralsMD/Code/Libraries/Source/WWVegas/WW3D2/ww3d.cpp|=WW3D::SyncTime = 0"
    "GeneralsMD/Code/Libraries/Source/WWVegas/WW3D2/ww3d.cpp|=WW3D::PreviousSyncTime = 0"
    "GeneralsMD/Code/Libraries/Source/WWVegas/WW3D2/ww3d.cpp|=WW3D::FractionalSyncMs = 0.0f"
    "GeneralsMD/Code/Libraries/Source/WWVegas/WW3D2/ww3d.cpp|=WW3D::LogicFrameTimeMs = 1000.0f / WWSyncPerSecond"
    "Generals/Code/GameEngine/Source/GameClient/Drawable.cpp|Drawable::Drawable"
    "Generals/Code/GameEngineDevice/Source/SDL3GameEngine.cpp|SDL3GameEngine::createGameLogic"
    "Generals/Code/GameEngineDevice/Source/SDL3GameEngine.cpp|SDL3GameEngine::createModuleFactory"
    "Generals/Code/GameEngineDevice/Source/SDL3GameEngine.cpp|SDL3GameEngine::createThingFactory"
    "Generals/Code/GameEngineDevice/Source/SDL3GameEngine.cpp|SDL3GameEngine::createFunctionLexicon"
    "Generals/Code/GameEngineDevice/Source/SDL3GameEngine.cpp|SDL3GameEngine::createLocalFileSystem"
    "Generals/Code/GameEngineDevice/Source/SDL3GameEngine.cpp|SDL3GameEngine::createArchiveFileSystem"
    "Generals/Code/Libraries/Source/WWVegas/WW3D2/ww3d.cpp|WW3D::Sync"
    "Generals/Code/Libraries/Source/WWVegas/WW3D2/ww3d.cpp|WW3D::Update_Logic_Frame_Time"
    "Generals/Code/Libraries/Source/WWVegas/WW3D2/ww3d.cpp|=WW3D::SyncTime = 0"
    "Generals/Code/Libraries/Source/WWVegas/WW3D2/ww3d.cpp|=WW3D::PreviousSyncTime = 0"
    "Generals/Code/Libraries/Source/WWVegas/WW3D2/ww3d.cpp|=WW3D::FractionalSyncMs = 0.0f"
    "Generals/Code/Libraries/Source/WWVegas/WW3D2/ww3d.cpp|=WW3D::LogicFrameTimeMs = 1000.0f / WWSyncPerSecond"
    "Core/GameEngineDevice/Source/W3DDevice/GameClient/W3DTerrainVisual.cpp|W3DTerrainVisual::update"
    "Core/GameEngineDevice/Source/W3DDevice/GameClient/Water/W3DWater.cpp|=#define WATER_MESH_X_VERTICES"
    "Core/GameEngineDevice/Source/W3DDevice/GameClient/Water/W3DWater.cpp|=if( m_doWaterGrid && m_meshInMotion == TRUE && gxAdvanced )"
    "Core/GameEngineDevice/Source/W3DDevice/GameClient/BaseHeightMap.cpp|BaseHeightMapRenderObjClass::getHeightMapHeight"
    "Core/GameEngineDevice/Source/W3DDevice/GameClient/BaseHeightMap.cpp|BaseHeightMapRenderObjClass::isClearLineOfSight"
    "Core/GameEngineDevice/Source/W3DDevice/GameClient/BaseHeightMap.cpp|BaseHeightMapRenderObjClass::isCliffCell"
    "Core/GameEngineDevice/Source/W3DDevice/GameClient/BaseHeightMap.cpp|BaseHeightMapRenderObjClass::BaseHeightMapRenderObjClass"
    "Core/GameEngineDevice/Source/W3DDevice/GameClient/BaseHeightMap.cpp|BaseHeightMapRenderObjClass::loadRoadsAndBridges"
    "Core/GameEngineDevice/Source/W3DDevice/GameClient/W3DTerrainVisual.cpp|=m_terrainRenderObject = NEW_REF( HeightMapRenderObjClass, () )"
    "Core/GameEngineDevice/Source/W3DDevice/GameClient/W3DTerrainVisual.cpp|=TheWaterRenderObj=m_waterRenderObject = NEW_REF( WaterRenderObjClass, () )"
    "Core/GameEngineDevice/Source/W3DDevice/GameClient/W3DTerrainVisual.cpp|W3DTerrainVisual::load"
    "Core/GameEngineDevice/Source/W3DDevice/GameClient/W3DTerrainVisual.cpp|W3DTerrainVisual::enableWaterGrid"
    "Core/GameEngineDevice/Source/W3DDevice/GameClient/W3DTerrainVisual.cpp|W3DTerrainVisual::getTerrainTile"
    "Core/GameEngineDevice/Source/W3DDevice/GameClient/W3DTerrainVisual.cpp|W3DTerrainVisual::setWaterGridHeightClamps"
    "Core/GameEngineDevice/Source/W3DDevice/GameClient/W3DTerrainVisual.cpp|W3DTerrainVisual::setWaterAttenuationFactors"
    "Core/GameEngineDevice/Source/W3DDevice/GameClient/W3DTerrainVisual.cpp|W3DTerrainVisual::setWaterTransform"
    "Core/GameEngineDevice/Source/W3DDevice/GameClient/W3DTerrainVisual.cpp|W3DTerrainVisual::getWaterTransform"
    "Core/GameEngineDevice/Source/W3DDevice/GameClient/W3DTerrainVisual.cpp|W3DTerrainVisual::setWaterGridResolution"
    "Core/GameEngineDevice/Source/W3DDevice/GameClient/W3DTerrainVisual.cpp|W3DTerrainVisual::getWaterGridResolution"
    "Core/GameEngineDevice/Source/W3DDevice/GameClient/W3DTerrainVisual.cpp|W3DTerrainVisual::changeWaterHeight"
    "Core/GameEngineDevice/Source/W3DDevice/GameClient/W3DTerrainVisual.cpp|W3DTerrainVisual::addWaterVelocity"
    "Core/GameEngineDevice/Source/W3DDevice/GameClient/W3DTerrainVisual.cpp|W3DTerrainVisual::getWaterGridHeight"
    "Core/GameEngineDevice/Source/W3DDevice/GameClient/W3DTerrainVisual.cpp|W3DTerrainVisual::setRawMapHeight"
    "Core/GameEngineDevice/Source/W3DDevice/GameClient/W3DTerrainVisual.cpp|W3DTerrainVisual::getRawMapHeight"
    "Core/GameEngineDevice/Source/W3DDevice/GameClient/Water/W3DWater.cpp|WaterRenderObjClass::WaterRenderObjClass"
    "Core/GameEngineDevice/Source/W3DDevice/GameClient/Water/W3DWater.cpp|WaterRenderObjClass::reset"
    "Core/GameEngineDevice/Source/W3DDevice/GameClient/Water/W3DWater.cpp|WaterRenderObjClass::enableWaterGrid"
    "Core/GameEngineDevice/Source/W3DDevice/GameClient/Water/W3DWater.cpp|WaterRenderObjClass::setGridVertexHeight"
    "Core/GameEngineDevice/Source/W3DDevice/GameClient/Water/W3DWater.cpp|WaterRenderObjClass::setGridHeightClamps"
    "Core/GameEngineDevice/Source/W3DDevice/GameClient/Water/W3DWater.cpp|WaterRenderObjClass::addVelocity"
    "Core/GameEngineDevice/Source/W3DDevice/GameClient/Water/W3DWater.cpp|WaterRenderObjClass::changeGridHeight"
    "Core/GameEngineDevice/Source/W3DDevice/GameClient/Water/W3DWater.cpp|WaterRenderObjClass::setGridChangeAttenuationFactors"
    "Core/GameEngineDevice/Source/W3DDevice/GameClient/Water/W3DWater.cpp|WaterRenderObjClass::setGridTransform"
    "Core/GameEngineDevice/Source/W3DDevice/GameClient/Water/W3DWater.cpp|WaterRenderObjClass::getGridTransform"
    "Core/GameEngineDevice/Source/W3DDevice/GameClient/Water/W3DWater.cpp|WaterRenderObjClass::setGridResolution"
    "Core/GameEngineDevice/Source/W3DDevice/GameClient/Water/W3DWater.cpp|WaterRenderObjClass::getGridResolution"
    "Core/GameEngineDevice/Source/W3DDevice/GameClient/Drawable/Draw/W3DModelDraw.cpp|isValidTimeToCalcLogicStuff"
    "Core/GameEngineDevice/Source/W3DDevice/GameClient/Drawable/Draw/W3DModelDraw.cpp|parseAsciiStringLC"
    "Core/GameEngineDevice/Source/W3DDevice/GameClient/Drawable/Draw/W3DModelDraw.cpp|=static const char *const ACBitsNames[] ="
    "Core/GameEngineDevice/Source/W3DDevice/GameClient/Drawable/Draw/W3DModelDraw.cpp|=enum ACBits CPP_11(: Int)"
    "Core/GameEngineDevice/Source/W3DDevice/GameClient/Drawable/Draw/W3DModelDraw.cpp|testFlagBit"
    "Core/GameEngineDevice/Source/W3DDevice/GameClient/Drawable/Draw/W3DModelDraw.cpp|findSingleBone"
    "Core/GameEngineDevice/Source/W3DDevice/GameClient/Drawable/Draw/W3DModelDraw.cpp|findSingleSubObj"
    "Core/GameEngineDevice/Source/W3DDevice/GameClient/Drawable/Draw/W3DModelDraw.cpp|doSingleBoneName"
    "Core/GameEngineDevice/Source/W3DDevice/GameClient/Drawable/Draw/W3DModelDraw.cpp|W3DAnimationInfo::getAnimHandle"
    "Core/GameEngineDevice/Source/W3DDevice/GameClient/Drawable/Draw/W3DModelDraw.cpp|ModelConditionInfo::addPublicBone"
    "Core/GameEngineDevice/Source/W3DDevice/GameClient/Drawable/Draw/W3DModelDraw.cpp|ModelConditionInfo::matchesMode"
    "Core/GameEngineDevice/Source/W3DDevice/GameClient/Drawable/Draw/W3DModelDraw.cpp|ModelConditionInfo::validateStuff"
    "Core/GameEngineDevice/Source/W3DDevice/GameClient/Drawable/Draw/W3DModelDraw.cpp|ModelConditionInfo::validateCachedBones"
    "Core/GameEngineDevice/Source/W3DDevice/GameClient/Drawable/Draw/W3DModelDraw.cpp|ModelConditionInfo::validateWeaponBarrelInfo"
    "Core/GameEngineDevice/Source/W3DDevice/GameClient/Drawable/Draw/W3DModelDraw.cpp|ModelConditionInfo::validateTurretInfo"
    "Core/GameEngineDevice/Source/W3DDevice/GameClient/Drawable/Draw/W3DModelDraw.cpp|ModelConditionInfo::findPristineBone"
    "Core/GameEngineDevice/Source/W3DDevice/GameClient/Drawable/Draw/W3DModelDraw.cpp|ModelConditionInfo::findPristineBonePos"
    "Core/GameEngineDevice/Source/W3DDevice/GameClient/Drawable/Draw/W3DModelDraw.cpp|ModelConditionInfo::loadAnimations"
    "Core/GameEngineDevice/Source/W3DDevice/GameClient/Drawable/Draw/W3DModelDraw.cpp|ModelConditionInfo::clear"
    "Core/GameEngineDevice/Source/W3DDevice/GameClient/Drawable/Draw/W3DModelDraw.cpp|W3DModelDrawModuleData::W3DModelDrawModuleData"
    "Core/GameEngineDevice/Source/W3DDevice/GameClient/Drawable/Draw/W3DModelDraw.cpp|W3DModelDrawModuleData::validateStuffForTimeAndWeather"
    "Core/GameEngineDevice/Source/W3DDevice/GameClient/Drawable/Draw/W3DModelDraw.cpp|W3DModelDrawModuleData::buildFieldParse"
    "Core/GameEngineDevice/Source/W3DDevice/GameClient/Drawable/Draw/W3DModelDraw.cpp|parseAnimation"
    "Core/GameEngineDevice/Source/W3DDevice/GameClient/Drawable/Draw/W3DModelDraw.cpp|parseShowHideSubObject"
    "Core/GameEngineDevice/Source/W3DDevice/GameClient/Drawable/Draw/W3DModelDraw.cpp|parseWeaponBoneName"
    "Core/GameEngineDevice/Source/W3DDevice/GameClient/Drawable/Draw/W3DModelDraw.cpp|parseParticleSysBone"
    "Core/GameEngineDevice/Source/W3DDevice/GameClient/Drawable/Draw/W3DModelDraw.cpp|parseRealRange"
    "Core/GameEngineDevice/Source/W3DDevice/GameClient/Drawable/Draw/W3DModelDraw.cpp|parseLowercaseNameKey"
    "Core/GameEngineDevice/Source/W3DDevice/GameClient/Drawable/Draw/W3DModelDraw.cpp|parseBoneNameKey"
    "Core/GameEngineDevice/Source/W3DDevice/GameClient/Drawable/Draw/W3DModelDraw.cpp|doesStateExist"
    "Core/GameEngineDevice/Source/W3DDevice/GameClient/Drawable/Draw/W3DModelDraw.cpp|W3DModelDrawModuleData::parseConditionState"
    "Core/GameEngineDevice/Source/W3DDevice/GameClient/Drawable/Draw/W3DModelDraw.cpp|W3DModelDrawModuleData::findBestInfo"
    "Core/GameEngineDevice/Source/W3DDevice/GameClient/Drawable/Draw/W3DModelDraw.cpp|W3DModelDraw::W3DModelDraw"
    "Core/GameEngineDevice/Source/W3DDevice/GameClient/Drawable/Draw/W3DModelDraw.cpp|W3DModelDraw::onDrawableBoundToObject"
    "Core/GameEngineDevice/Source/W3DDevice/GameClient/Drawable/Draw/W3DModelDraw.cpp|W3DModelDraw::setModelState"
    "Core/GameEngineDevice/Source/W3DDevice/GameClient/Drawable/Draw/W3DModelDraw.cpp|W3DModelDraw::replaceModelConditionState"
    "Core/GameEngineDevice/Source/W3DDevice/GameClient/Drawable/Draw/W3DModelDraw.cpp|W3DModelDraw::getProjectileLaunchOffset"
    "Core/GameEngineDevice/Source/W3DDevice/GameClient/Drawable/Draw/W3DModelDraw.cpp|W3DModelDraw::getPristineBonePositionsForConditionState"
    "Core/GameEngineDevice/Source/W3DDevice/GameClient/Drawable/Draw/W3DModelDraw.cpp|W3DModelDraw::findBestInfo"
    "Core/GameEngineDevice/Source/W3DDevice/GameClient/Drawable/Draw/W3DModelDraw.cpp|W3DModelDraw::getBarrelCount"
    "Core/GameEngineDevice/Source/W3DDevice/GameClient/Drawable/Draw/W3DModelDraw.cpp|W3DModelDraw::updateDrawModuleSupplyStatus"
    "Core/GameEngineDevice/Source/W3DDevice/GameClient/Drawable/Draw/W3DLaserDraw.cpp|W3DLaserDrawModuleData::W3DLaserDrawModuleData"
    "Core/GameEngineDevice/Source/W3DDevice/GameClient/Drawable/Draw/W3DLaserDraw.cpp|W3DLaserDrawModuleData::buildFieldParse"
    "Core/GameEngineDevice/Source/W3DDevice/GameClient/Drawable/Draw/W3DLaserDraw.cpp|W3DLaserDraw::getLaserTemplateWidth"
    "Core/GameEngine/Source/GameClient/GUI/ControlBar/ControlBar.cpp|=const FieldParse CommandButton::s_commandButtonFieldParseTable[] ="
    "Core/GameEngine/Source/GameClient/GUI/ControlBar/ControlBar.cpp|=const FieldParse CommandSet::m_commandSetFieldParseTable[] ="
    "Core/GameEngine/Source/GameClient/GUI/ControlBar/ControlBar.cpp|CommandButton::parseCommand"
    "Core/GameEngine/Source/GameClient/GUI/ControlBar/ControlBar.cpp|CommandButton::CommandButton"
    "Core/GameEngine/Source/GameClient/GUI/ControlBar/ControlBar.cpp|CommandButton::isValidRelationshipTarget"
    "Core/GameEngine/Source/GameClient/GUI/ControlBar/ControlBar.cpp|CommandButton::isValidObjectTarget"
    "Core/GameEngine/Source/GameClient/GUI/ControlBar/ControlBar.cpp|CommandButton::isValidToUseOn"
    "Core/GameEngine/Source/GameClient/GUI/ControlBar/ControlBar.cpp|CommandButton::isReady"
    "Core/GameEngine/Source/GameClient/GUI/ControlBar/ControlBar.cpp|CommandButton::isContextCommand"
    "Core/GameEngine/Source/GameClient/GUI/ControlBar/ControlBar.cpp|CommandSet::parseCommandButton"
    "Core/GameEngine/Source/GameClient/GUI/ControlBar/ControlBar.cpp|ControlBar::parseCommandSetDefinition"
    "Core/GameEngine/Source/GameClient/GUI/ControlBar/ControlBar.cpp|=ini.loadFileDirectory("
    "Core/GameEngine/Source/GameClient/GUI/ControlBar/ControlBar.cpp|ControlBar::postProcessCommands"
    "Core/GameEngine/Source/GameClient/GUI/ControlBar/ControlBar.cpp|CommandSet::CommandSet"
    "Core/GameEngine/Source/GameClient/GUI/ControlBar/ControlBar.cpp|CommandSet::getCommandButton"
    "Core/GameEngine/Source/GameClient/GUI/ControlBar/ControlBar.cpp|ControlBar::findNonConstCommandButton"
    "Core/GameEngine/Source/GameClient/GUI/ControlBar/ControlBar.cpp|ControlBar::newCommandButton"
    "Core/GameEngine/Source/GameClient/GUI/ControlBar/ControlBar.cpp|ControlBar::newCommandButtonOverride"
    "Core/GameEngine/Source/GameClient/GUI/ControlBar/ControlBar.cpp|ControlBar::findNonConstCommandSet"
    "Core/GameEngine/Source/GameClient/GUI/ControlBar/ControlBar.cpp|ControlBar::findCommandButton"
    "Core/GameEngine/Source/GameClient/GUI/ControlBar/ControlBar.cpp|ControlBar::findCommandSet"
    "Core/GameEngine/Source/GameClient/GUI/ControlBar/ControlBar.cpp|ControlBar::newCommandSet"
    "Core/GameEngine/Source/GameClient/GUI/ControlBar/ControlBar.cpp|ControlBar::newCommandSetOverride"
    # det-codex r9: ParticleUplinkCannonUpdate finds its beam's LaserUpdate through GameClient::findDrawableByID and
    # Drawable::findClientUpdateModule and takes the damage radius from it - the lookup, its table and its accessors.
    "GeneralsMD/Code/GameEngine/Source/GameClient/GameClient.cpp|GameClient::addDrawableToLookupTable"
    "GeneralsMD/Code/GameEngine/Source/GameClient/GameClient.cpp|GameClient::removeDrawableFromLookupTable"
    "GeneralsMD/Code/GameEngine/Source/GameClient/GameClient.cpp|=m_drawableVector.clear()"
    "GeneralsMD/Code/GameEngine/Source/GameClient/GameClient.cpp|=m_drawableVector.resize(DRAWABLE_HASH_SIZE, nullptr)"
    "Generals/Code/GameEngine/Source/GameClient/GameClient.cpp|GameClient::findDrawableByID"
    "Generals/Code/GameEngine/Source/GameClient/GameClient.cpp|GameClient::addDrawableToLookupTable"
    "Generals/Code/GameEngine/Source/GameClient/GameClient.cpp|GameClient::removeDrawableFromLookupTable"
    "Generals/Code/GameEngine/Source/GameClient/GameClient.cpp|=m_drawableHash.clear()"
    "Generals/Code/GameEngine/Source/GameClient/GameClient.cpp|=m_drawableHash.reserve(DRAWABLE_HASH_SIZE)"
    "GeneralsMD/Code/GameEngine/Source/GameClient/Drawable.cpp|Drawable::findClientUpdateModule"
    "Generals/Code/GameEngine/Source/GameClient/Drawable.cpp|Drawable::findClientUpdateModule"
    # det-codex r10: how the beam drawable gets the ID the cannon stores and looks up - registration and the counter.
    "GeneralsMD/Code/GameEngine/Source/GameClient/GameClient.cpp|GameClient::registerDrawable"
    "GeneralsMD/Code/GameEngine/Source/GameClient/GameClient.cpp|GameClient::allocDrawableID"
    "GeneralsMD/Code/GameEngine/Source/GameClient/GameClient.cpp|=m_nextDrawableID = (DrawableID)1"
    "GeneralsMD/Code/GameEngine/Source/GameClient/Drawable.cpp|Drawable::setID"
    "GeneralsMD/Code/GameEngine/Source/GameClient/Drawable.cpp|Drawable::getID"
    "Generals/Code/GameEngine/Source/GameClient/GameClient.cpp|GameClient::registerDrawable"
    "Generals/Code/GameEngine/Source/GameClient/GameClient.cpp|GameClient::allocDrawableID"
    "Generals/Code/GameEngine/Source/GameClient/GameClient.cpp|=m_nextDrawableID = (DrawableID)1"
    "Generals/Code/GameEngine/Source/GameClient/Drawable.cpp|Drawable::setID"
    "Generals/Code/GameEngine/Source/GameClient/Drawable.cpp|Drawable::getID"
    "GeneralsMD/Code/GameEngine/Source/GameClient/Drawable.cpp|Drawable::getProjectileLaunchOffset"
    "GeneralsMD/Code/GameEngine/Source/GameClient/Drawable.cpp|Drawable::getPristineBonePositions"
    "GeneralsMD/Code/GameEngine/Source/GameClient/Drawable.cpp|Drawable::getScale"
    "GeneralsMD/Code/GameEngine/Source/GameClient/Drawable.cpp|Drawable::reactToBodyDamageStateChange"
    "GeneralsMD/Code/GameEngine/Source/GameClient/Drawable.cpp|Drawable::clearAndSetModelConditionState"
    "GeneralsMD/Code/GameEngine/Source/GameClient/Drawable.cpp|Drawable::getDrawModules"
    "GeneralsMD/Code/GameEngine/Source/GameClient/Drawable.cpp|Drawable::clearAndSetModelConditionFlags"
    "GeneralsMD/Code/GameEngine/Source/GameClient/Drawable.cpp|Drawable::replaceModelConditionFlags"
    "GeneralsMD/Code/GameEngine/Source/GameClient/Drawable.cpp|Drawable::replaceModelConditionStateInDrawable"
    "GeneralsMD/Code/GameEngine/Source/GameClient/Drawable.cpp|Drawable::getDrawableGeometryInfo"
    "GeneralsMD/Code/GameEngine/Source/GameClient/Drawable.cpp|Drawable::getBarrelCount"
    "GeneralsMD/Code/GameEngine/Source/GameClient/Drawable.cpp|Drawable::setInstanceMatrix"
    "GeneralsMD/Code/GameEngine/Source/GameClient/Drawable.cpp|Drawable::setTimeOfDay"
    "GeneralsMD/Code/GameEngine/Source/GameClient/Drawable.cpp|Drawable::updateDrawableSupplyStatus"
    "GeneralsMD/Code/GameEngine/Source/GameClient/Drawable.cpp|Drawable::friend_lockDirtyStuffForIteration"
    "GeneralsMD/Code/GameEngine/Source/GameClient/Drawable.cpp|Drawable::friend_unlockDirtyStuffForIteration"
    "GeneralsMD/Code/GameEngine/Source/GameClient/Drawable.cpp|Drawable::getDrawModulesNonDirty"
    "GeneralsMD/Code/GameEngineDevice/Source/W3DDevice/GameClient/W3DDisplay.cpp|=m_assetManager = NEW W3DAssetManager"
    "GeneralsMD/Code/GameEngineDevice/Source/W3DDevice/GameClient/W3DBridgeBuffer.cpp|W3DBridge::init"
    "GeneralsMD/Code/GameEngineDevice/Source/W3DDevice/GameClient/W3DBridgeBuffer.cpp|W3DBridge::load"
    "GeneralsMD/Code/GameEngineDevice/Source/W3DDevice/GameClient/W3DBridgeBuffer.cpp|W3DBridge::getBridgeInfo"
    "GeneralsMD/Code/GameEngineDevice/Source/W3DDevice/GameClient/W3DBridgeBuffer.cpp|W3DBridgeBuffer::loadBridges"
    "GeneralsMD/Code/GameEngineDevice/Source/W3DDevice/GameClient/W3DBridgeBuffer.cpp|liveGameWithAnotherHuman"
    "GeneralsMD/Code/GameEngineDevice/Source/W3DDevice/GameClient/W3DBridgeBuffer.cpp|W3DBridgeBuffer::addBridge"
    "Generals/Code/GameEngine/Source/GameClient/Drawable.cpp|Drawable::getProjectileLaunchOffset"
    "Generals/Code/GameEngine/Source/GameClient/Drawable.cpp|Drawable::getPristineBonePositions"
    "Generals/Code/GameEngine/Source/GameClient/Drawable.cpp|Drawable::getScale"
    "Generals/Code/GameEngine/Source/GameClient/Drawable.cpp|Drawable::reactToBodyDamageStateChange"
    "Generals/Code/GameEngine/Source/GameClient/Drawable.cpp|Drawable::clearAndSetModelConditionState"
    "Generals/Code/GameEngine/Source/GameClient/Drawable.cpp|Drawable::getDrawModules"
    "Generals/Code/GameEngine/Source/GameClient/Drawable.cpp|Drawable::clearAndSetModelConditionFlags"
    "Generals/Code/GameEngine/Source/GameClient/Drawable.cpp|Drawable::replaceModelConditionFlags"
    "Generals/Code/GameEngine/Source/GameClient/Drawable.cpp|Drawable::replaceModelConditionStateInDrawable"
    "Generals/Code/GameEngine/Source/GameClient/Drawable.cpp|Drawable::getDrawableGeometryInfo"
    "Generals/Code/GameEngine/Source/GameClient/Drawable.cpp|Drawable::getBarrelCount"
    "Generals/Code/GameEngine/Source/GameClient/Drawable.cpp|Drawable::setInstanceMatrix"
    "Generals/Code/GameEngine/Source/GameClient/Drawable.cpp|Drawable::setTimeOfDay"
    "Generals/Code/GameEngine/Source/GameClient/Drawable.cpp|Drawable::updateDrawableSupplyStatus"
    "Generals/Code/GameEngine/Source/GameClient/Drawable.cpp|Drawable::friend_lockDirtyStuffForIteration"
    "Generals/Code/GameEngine/Source/GameClient/Drawable.cpp|Drawable::friend_unlockDirtyStuffForIteration"
    "Generals/Code/GameEngineDevice/Source/W3DDevice/GameClient/W3DDisplay.cpp|=m_assetManager = NEW W3DAssetManager"
    "Generals/Code/GameEngineDevice/Source/W3DDevice/GameClient/W3DBridgeBuffer.cpp|W3DBridge::init"
    "Generals/Code/GameEngineDevice/Source/W3DDevice/GameClient/W3DBridgeBuffer.cpp|W3DBridge::load"
    "Generals/Code/GameEngineDevice/Source/W3DDevice/GameClient/W3DBridgeBuffer.cpp|W3DBridge::getBridgeInfo"
    "Generals/Code/GameEngineDevice/Source/W3DDevice/GameClient/W3DBridgeBuffer.cpp|W3DBridgeBuffer::loadBridges"
    "Generals/Code/GameEngineDevice/Source/W3DDevice/GameClient/W3DBridgeBuffer.cpp|liveGameWithAnotherHuman"
    "Generals/Code/GameEngineDevice/Source/W3DDevice/GameClient/W3DBridgeBuffer.cpp|W3DBridgeBuffer::addBridge"
)

# A git *failure* must never look like a git *absence*: absence yields the reserved 0
# sentinel, failure stops the build. Without this, a broken git (an xcrun shim after a
# macOS update, or "dubious ownership" in a container) makes all three plumbing calls
# print nothing while still exiting non-zero, the tree text is empty, and the digest
# becomes a fixed non-zero constant identical on every machine in that state - a
# silent permanent "compatible".
macro(SimDigestGit out_var)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -c core.quotepath=off ${ARGN}
        WORKING_DIRECTORY "${SIMDIGEST_WORKING_DIR}"
        RESULT_VARIABLE _sd_exit
        OUTPUT_VARIABLE ${out_var}
        ERROR_VARIABLE _sd_err
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT _sd_exit EQUAL 0)
        message(FATAL_ERROR
            "SimID: git is present but 'git ${ARGN}' failed (exit ${_sd_exit}) in "
            "${SIMDIGEST_WORKING_DIR}:\n${_sd_err}\n"
            "Refusing to emit a fake source digest - two broken builds would then "
            "report the same value and be allowed to play. Fix git, or configure "
            "with -DSIMDIGEST_ALLOW_NO_GIT=ON to build with the 'unknown provenance' "
            "sentinel instead.")
    endif()
endmacro()

function(SimDigestCollect _out_text _regex)
    set(_paths ${ARGN})

    SimDigestGit(_tree ls-tree -r --full-tree HEAD -- ${_paths})
    string(REPLACE "\n" ";" _tree_lines "${_tree}")
    list(FILTER _tree_lines INCLUDE REGEX "${_regex}")
    list(SORT _tree_lines)

    if(_tree_lines STREQUAL "")
        message(FATAL_ERROR
            "SimID: the path list matched no tracked files under ${SIMDIGEST_WORKING_DIR}. "
            "The digest would be a fixed constant on every machine. Path list: ${_paths}")
    endif()

    SimDigestGit(_changed diff-index --name-only HEAD -- ${_paths})
    SimDigestGit(_untracked ls-files --others --exclude-standard -- ${_paths})

    set(_dirty "")
    if(NOT _changed STREQUAL "")
        string(REPLACE "\n" ";" _c "${_changed}")
        list(APPEND _dirty ${_c})
    endif()
    if(NOT _untracked STREQUAL "")
        string(REPLACE "\n" ";" _u "${_untracked}")
        list(APPEND _dirty ${_u})
    endif()
    if(NOT _dirty STREQUAL "")
        list(FILTER _dirty INCLUDE REGEX "${_regex}")
        list(REMOVE_DUPLICATES _dirty)
        list(SORT _dirty)
    endif()

    set(_overlay "")
    set(_overlay_count 0)
    foreach(_p IN LISTS _dirty)
        if(EXISTS "${SIMDIGEST_WORKING_DIR}/${_p}")
            execute_process(
                COMMAND "${GIT_EXECUTABLE}" hash-object -- "${_p}"
                WORKING_DIRECTORY "${SIMDIGEST_WORKING_DIR}"
                RESULT_VARIABLE _hx
                OUTPUT_VARIABLE _sha
                ERROR_VARIABLE _herr
                OUTPUT_STRIP_TRAILING_WHITESPACE)
            if(NOT _hx EQUAL 0)
                message(FATAL_ERROR "SimID: 'git hash-object -- ${_p}' failed (exit ${_hx}): ${_herr}")
            endif()
        else()
            set(_sha "DELETED")
        endif()
        # Only record a real difference. This discards the two pseudo-modifications
        # that differ by platform rather than by content: a core.fileMode exec-bit
        # flip (true on macOS, false on Windows) and a stale-index eol renormalisation.
        string(FIND "${_tree}" " ${_sha}\t${_p}" _found)
        if(_found EQUAL -1)
            string(APPEND _overlay "${_p}\t${_sha}\n")
            math(EXPR _overlay_count "${_overlay_count} + 1")
        endif()
    endforeach()

    string(REPLACE ";" "\n" _tree_text "${_tree_lines}")
    set(${_out_text} "${_tree_text}\n#dirty\n${_overlay}" PARENT_SCOPE)
    set(${_out_text}_DIRTY ${_overlay_count} PARENT_SCOPE)
endfunction()

# _text as code: escapes, then character and string literals, then line and block comments go first - a heuristic.
function(SimDigestCodeOnly _out _text)
    string(REGEX REPLACE "\\\\." "__" _code "${_text}")
    string(REGEX REPLACE "'[^'\n]*'" "''" _code "${_code}")
    string(REGEX REPLACE "\"[^\"\n]*\"" "\"\"" _code "${_code}")
    string(REGEX REPLACE "//[^\n]*" "" _code "${_code}")
    string(REGEX REPLACE "/\\*([^*]|\\*+[^*/])*\\*+/" "" _code "${_code}")
    set(${_out} "${_code}" PARENT_SCOPE)
endfunction()

# The text of every GX-SIMID-BEGIN .. GX-SIMID-END region of one file, markers included, CRLF normalised to LF so a
# Windows autocrlf working tree hashes like the committed blob. On a malformed file _err is set and _out is empty.
function(SimDigestRegionText _out _outside _err _path _src)
    string(REPLACE "\r\n" "\n" _rest "${_src}")
    set(_acc "")
    set(_off "")
    set(_count 0)
    set(${_out} "" PARENT_SCOPE)
    set(${_outside} "" PARENT_SCOPE)
    while(1)	# not TRUE: under an unset CMP0012 a cmake -P run on CMake 3.x reads TRUE as a variable
        string(FIND "${_rest}" "GX-SIMID-BEGIN" _b)
        string(FIND "${_rest}" "GX-SIMID-END" _e)
        if(_b EQUAL -1)
            if(NOT _e EQUAL -1)
                set(${_err} "${_path}: a GX-SIMID-END with no GX-SIMID-BEGIN before it" PARENT_SCOPE)
                return()
            endif()
            string(APPEND _off "${_rest}")
            break()
        endif()
        if(_e EQUAL -1 OR _e LESS _b)
            set(${_err} "${_path}: a GX-SIMID-BEGIN never closed, or an END before it" PARENT_SCOPE)
            return()
        endif()
        math(EXPR _len "${_e} - ${_b}")
        string(SUBSTRING "${_rest}" ${_b} ${_len} _region)
        string(FIND "${_region}" "GX-SIMID-BEGIN" _last REVERSE)
        if(NOT _last EQUAL 0)
            set(${_err} "${_path}: nested GX-SIMID-BEGIN" PARENT_SCOPE)
            return()
        endif()
        # A region holds whole definitions: something besides the marker lines, and as many { as } - a marker moved
        # into a function body leaves part of it outside, and this is what notices.
        string(FIND "${_region}" "\n" _nl)
        string(FIND "${_region}" "\n" _lastnl REVERSE)
        set(_body "")
        if(_lastnl GREATER _nl)
            math(EXPR _blen "${_lastnl} - ${_nl}")
            string(SUBSTRING "${_region}" ${_nl} ${_blen} _body)
        endif()
        string(STRIP "${_body}" _body)
        if(_body STREQUAL "")
            set(${_err} "${_path}: an empty GX-SIMID region" PARENT_SCOPE)
            return()
        endif()
        # Braces of code only (Codex det-codex-r27: a '}' in a comment or a string literal refused valid code).
        SimDigestCodeOnly(_code "${_region}")
        string(LENGTH "${_code}" _l0)
        string(REPLACE "{" "" _nob "${_code}")
        string(LENGTH "${_nob}" _l1)
        string(REPLACE "}" "" _noc "${_code}")
        string(LENGTH "${_noc}" _l2)
        if(NOT _l1 EQUAL _l2)
            math(EXPR _ob "${_l0} - ${_l1}")
            math(EXPR _cb "${_l0} - ${_l2}")
            set(${_err} "${_path}: a GX-SIMID region with ${_ob} '{' but ${_cb} '}' - a marker sits inside a definition" PARENT_SCOPE)
            return()
        endif()
        # ...and they must PAIR, not just count equal (Codex det-codex-r29): a region from the middle of one function to
        # the opening brace of the next reads '} ... {', balanced, while the second body stays outside it.
        string(REGEX REPLACE "[^{}]" "" _br "${_code}")
        set(_was "")
        while(NOT _br STREQUAL _was)
            set(_was "${_br}")
            string(REPLACE "{}" "" _br "${_br}")
        endwhile()
        if(NOT _br STREQUAL "")
            set(${_err} "${_path}: a GX-SIMID region whose braces do not pair up ('${_br}' left) - a marker sits inside a definition" PARENT_SCOPE)
            return()
        endif()
        string(SUBSTRING "${_rest}" 0 ${_b} _before)
        string(APPEND _off "${_before}")
        string(APPEND _acc "${_region}\n")
        math(EXPR _count "${_count} + 1")
        math(EXPR _next "${_e} + 12")	# past "GX-SIMID-END"
        string(SUBSTRING "${_rest}" ${_next} -1 _rest)
    endwhile()
    if(_count EQUAL 0)
        set(${_err} "${_path}: on a SIMDIGEST_PATHS_REGION_* list but holds no GX-SIMID-BEGIN/END region" PARENT_SCOPE)
        return()
    endif()
    set(${_err} "" PARENT_SCOPE)
    set(${_out} "${_acc}" PARENT_SCOPE)
    set(${_outside} "${_off}" PARENT_SCOPE)
endfunction()

# The guard that keeps the regions from rotting: every entry of SIMDIGEST_REGION_FUNCTIONS names a hot file and either a
# definition - Class::function or a free function, matched as a line starting at column 0 that names it before a ( - or,
# after a '=', a literal text. Each must occur INSIDE a region of that file and nowhere outside one, every overload
# included, or the build stops: a function moved, renamed or left out of its markers would otherwise leave the digest
# quietly.
function(SimDigestCheckRegionFunctions _path _inside _outside)
    foreach(_entry IN LISTS SIMDIGEST_REGION_FUNCTIONS)
        string(FIND "${_entry}" "|" _bar)
        string(SUBSTRING "${_entry}" 0 ${_bar} _file)
        if(NOT _file STREQUAL _path)
            continue()
        endif()
        math(EXPR _bar "${_bar} + 1")
        string(SUBSTRING "${_entry}" ${_bar} -1 _what)
        string(SUBSTRING "${_what}" 0 1 _kind)
        if(_kind STREQUAL "=")
            string(SUBSTRING "${_what}" 1 -1 _lit)
            string(FIND "${_inside}" "${_lit}" _in)
            string(FIND "${_outside}" "${_lit}" _out)
        else()
            string(REPLACE "~" "[~]" _rx "${_what}")
            set(_def "\n(/[*][^\n]*[*]/[ \t]*)?([A-Za-z_][^\n]*[^A-Za-z0-9_:])?${_rx}[ \t\n]*[(]")
            string(REGEX MATCH "${_def}" _out "\n${_outside}")
            # Every inside definition must reach its opening brace before the next region starts: an END marker moved
            # between a signature and its body keeps the region balanced and would leave the body unhashed (Codex r2).
            # One REGEX MATCH at a time on a shrinking string, never a MATCHALL list: a '[' inside a hit made CMake merge
            # list elements, and a search that then failed read as offset -1 (Codex r3).
            set(_scan "\n${_inside}")
            set(_in -1)
            while(1)
                string(REGEX MATCH "${_def}" _h "${_scan}")
                if(_h STREQUAL "")
                    break()
                endif()
                string(FIND "${_scan}" "${_h}" _at)
                if(_at EQUAL -1)
                    message(FATAL_ERROR "SimID: ${_path}: cannot locate the definition of '${_what}' it just matched")
                endif()
                string(LENGTH "${_h}" _hl)
                math(EXPR _at "${_at} + ${_hl}")
                string(SUBSTRING "${_scan}" ${_at} -1 _scan)
                # The body's '{' is searched in the rest of this definition's OWN region, as code (Codex det-codex-r29: a
                # '{' in a comment before a moved END passed for it), past the parameter list and every other (...)
                # group, so a brace in a default argument cannot pass either. Regions pair their braces (above), so the
                # body's '}' is in the region too. ponytail: a brace-initialised member in a constructor's init list
                # (': m{0}') still reads as the body; none of the guarded constructors has one - parse it if one does.
                string(FIND "${_scan}" "GX-SIMID-BEGIN" _nb)
                if(_nb EQUAL -1)
                    set(_own "${_scan}")
                else()
                    string(SUBSTRING "${_scan}" 0 ${_nb} _own)
                endif()
                SimDigestCodeOnly(_own "(${_own}")
                set(_was "")
                while(NOT _own STREQUAL _was)
                    set(_was "${_own}")
                    string(REGEX REPLACE "[(][^()]*[)]" "" _own "${_own}")
                endwhile()
                string(FIND "${_own}" "{" _ob)
                set(_op 0)
                if(NOT _ob EQUAL -1)
                    string(SUBSTRING "${_own}" 0 ${_ob} _pre)
                    string(FIND "${_pre}" "(" _op)
                endif()
                if(_ob EQUAL -1 OR NOT _op EQUAL -1)
                    message(FATAL_ERROR "SimID: ${_path}: '${_what}' starts inside a GX-SIMID region but its body is not in it")
                endif()
                set(_in 0)
            endwhile()
            if(_out STREQUAL "")
                set(_out -1)
            else()
                set(_out 0)
            endif()
        endif()
        if(_in EQUAL -1 OR NOT _out EQUAL -1)
            message(FATAL_ERROR "SimID: ${_path}: '${_what}' must sit inside a GX-SIMID region and nowhere outside one "
                "- it is on SIMDIGEST_REGION_FUNCTIONS because the simulation reads it. Re-mark it, or move it to the list "
                "of the file it now lives in.")
        endif()
    endforeach()
endfunction()

# The regions of every listed file as the build will compile them (the working tree), each prefixed by its path; the
# dirty count is the files whose regions differ from HEAD's, so a render-only edit elsewhere in them is not dirt.
function(SimDigestRegions _out_text)
    set(_text "")
    set(_dirty 0)
    foreach(_p IN LISTS ARGN)
        if(NOT EXISTS "${SIMDIGEST_WORKING_DIR}/${_p}")
            message(FATAL_ERROR "SimID: region file ${_p} does not exist under ${SIMDIGEST_WORKING_DIR}")
        endif()
        file(READ "${SIMDIGEST_WORKING_DIR}/${_p}" _src)
        SimDigestRegionText(_now _off _bad "${_p}" "${_src}")
        if(_bad)
            message(FATAL_ERROR "SimID: ${_bad}. A region file whose markers cannot be read would drop simulation code out "
                "of the digest; fix the markers - see the SIMDIGEST_PATHS_REGION_* note above.")
        endif()
        SimDigestCheckRegionFunctions("${_p}" "${_now}" "${_off}")
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" show "HEAD:${_p}"
            WORKING_DIRECTORY "${SIMDIGEST_WORKING_DIR}"
            RESULT_VARIABLE _hx
            OUTPUT_VARIABLE _head
            ERROR_QUIET)
        set(_was "")
        if(_hx EQUAL 0)
            SimDigestRegionText(_was _was_off _bad_head "${_p}" "${_head}")
        endif()
        if(NOT _hx EQUAL 0 OR NOT _was STREQUAL _now)
            math(EXPR _dirty "${_dirty} + 1")
        endif()
        string(APPEND _text "${_p}\n${_now}")
    endforeach()
    set(${_out_text} "${_text}" PARENT_SCOPE)
    set(${_out_text}_DIRTY ${_dirty} PARENT_SCOPE)
endfunction()

# Every tracked code file under Core/, GeneralsMD/ and Generals/ that carries a GX-SIMID-BEGIN must be on a region list,
# or its regions would be ignored without a word.
function(SimDigestCheckMarkerCensus)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -c core.quotepath=off grep -l -F "GX-SIMID-BEGIN" -- Core GeneralsMD Generals
        WORKING_DIRECTORY "${SIMDIGEST_WORKING_DIR}"
        RESULT_VARIABLE _gx
        OUTPUT_VARIABLE _marked
        ERROR_VARIABLE _gerr
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT _gx EQUAL 0 AND NOT _gx EQUAL 1)
        message(FATAL_ERROR "SimID: 'git grep GX-SIMID-BEGIN' failed (exit ${_gx}): ${_gerr}")
    endif()
    string(REPLACE "\n" ";" _marked "${_marked}")
    list(FILTER _marked INCLUDE REGEX "${SIMDIGEST_CODE_EXT_REGEX}")
    set(_listed ${SIMDIGEST_PATHS_REGION_SHARED} ${SIMDIGEST_PATHS_REGION_ZH} ${SIMDIGEST_PATHS_REGION_G})
    set(_guarded "")
    foreach(_entry IN LISTS SIMDIGEST_REGION_FUNCTIONS)
        string(FIND "${_entry}" "|" _bar)
        string(SUBSTRING "${_entry}" 0 ${_bar} _file)
        list(FIND _listed "${_file}" _at)
        if(_at EQUAL -1)
            message(FATAL_ERROR "SimID: SIMDIGEST_REGION_FUNCTIONS names ${_file}, which is on no SIMDIGEST_PATHS_REGION_* list.")
        endif()
        list(APPEND _guarded "${_file}")
    endforeach()
    foreach(_r IN LISTS _listed)
        list(FIND _guarded "${_r}" _at)
        if(_at EQUAL -1)
            message(FATAL_ERROR "SimID: region file ${_r} has no entry on SIMDIGEST_REGION_FUNCTIONS, so nothing checks its markers.")
        endif()
    endforeach()
    foreach(_m IN LISTS _marked)
        list(FIND _listed "${_m}" _at)
        if(_at EQUAL -1)
            message(FATAL_ERROR "SimID: ${_m} carries a GX-SIMID-BEGIN marker but is on no SIMDIGEST_PATHS_REGION_* "
                "list in resources/gitinfo/simsourcedigest_watcher.cmake, so its regions would not be digested.")
        endif()
    endforeach()
endfunction()

function(ComputeSimSourceDigest _out _code_list _region_list)
    set(_code_paths ${${_code_list}})

    if(SIMDIGEST_ALLOW_NO_GIT OR NOT GIT_EXECUTABLE OR NOT EXISTS "${SIMDIGEST_WORKING_DIR}/.git")
        set(${_out} "00000000" PARENT_SCOPE)
        set(${_out}_DIRTY 0 PARENT_SCOPE)
        return()
    endif()

    SimDigestCollect(_code "${SIMDIGEST_CODE_EXT_REGEX}" ${_code_paths})
    SimDigestCollect(_build "${SIMDIGEST_BUILD_EXT_REGEX}" ${SIMDIGEST_PATHS_BUILD})
    SimDigestRegions(_regions ${${_region_list}})

    string(SHA256 _h "SIMID/3\nCODE\n${_code}\nBUILD\n${_build}\nREGIONS\n${_regions}")
    string(SUBSTRING "${_h}" 0 8 _short)
    string(TOUPPER "${_short}" _short)
    set(${_out} "${_short}" PARENT_SCOPE)
    math(EXPR _dirty "${_code_DIRTY} + ${_build_DIRTY} + ${_regions_DIRTY}")
    set(${_out}_DIRTY ${_dirty} PARENT_SCOPE)
endfunction()

# Function: SetupSimDigestMonitoring
# Description: registers the build-time re-invocation of this script. Everything the
#              script-mode process needs is forwarded with -D; see the header note.
function(SetupSimDigestMonitoring)
    add_custom_target(check_sim_source_digest
        ALL
        DEPENDS ${SIMDIGEST_PRE_CONFIGURE_FILE}
        BYPRODUCTS ${SIMDIGEST_POST_CONFIGURE_FILE}
        COMMENT "Computing the SimID source digest..."
        COMMAND
            ${CMAKE_COMMAND}
            -DSIMDIGEST_BUILD_TIME=TRUE
            -DSIMDIGEST_WORKING_DIR=${SIMDIGEST_WORKING_DIR}
            -DGIT_EXECUTABLE=${GIT_EXECUTABLE}
            -DSIMDIGEST_ALLOW_NO_GIT=${SIMDIGEST_ALLOW_NO_GIT}
            -DSIMDIGEST_PRE_CONFIGURE_FILE=${SIMDIGEST_PRE_CONFIGURE_FILE}
            -DSIMDIGEST_POST_CONFIGURE_FILE=${SIMDIGEST_POST_CONFIGURE_FILE}
            -P "${CMAKE_CURRENT_LIST_FILE}")
endfunction()

# Function: Main
# Description: primary entry-point. Selects configure-time or build-time behaviour.
function(Main)
    if(SIMDIGEST_BUILD_TIME)
        set(_zh_code ${SIMDIGEST_PATHS_CODE_SHARED} ${SIMDIGEST_PATHS_CODE_ZH})
        set(_zh_regions ${SIMDIGEST_PATHS_REGION_SHARED} ${SIMDIGEST_PATHS_REGION_ZH})
        set(_g_code ${SIMDIGEST_PATHS_CODE_SHARED} ${SIMDIGEST_PATHS_CODE_G})
        set(_g_regions ${SIMDIGEST_PATHS_REGION_SHARED} ${SIMDIGEST_PATHS_REGION_G})
        if(NOT SIMDIGEST_ALLOW_NO_GIT AND GIT_EXECUTABLE AND EXISTS "${SIMDIGEST_WORKING_DIR}/.git")
            SimDigestCheckMarkerCensus()
        endif()
        ComputeSimSourceDigest(SIM_SOURCE_DIGEST_ZEROHOUR _zh_code _zh_regions)
        ComputeSimSourceDigest(SIM_SOURCE_DIGEST_GENERALS _g_code _g_regions)
        message(STATUS "SimID source digest: ZH=0x${SIM_SOURCE_DIGEST_ZEROHOUR} G=0x${SIM_SOURCE_DIGEST_GENERALS}")
        # GeneralsX @bugfix Claude 02/10/2026 How many dirty files each digest folded in (the overlay above), so
        # scripts/build/simid-digest.sh reports dirt from the watcher's own path lists instead of a copy of them that
        # drifts (Codex m1det-4c-r1 P2: the CMakeLists.txt added to SIMDIGEST_PATHS_BUILD were missing from the copy).
        message(STATUS "SimID dirty overlay: ZH=${SIM_SOURCE_DIGEST_ZEROHOUR_DIRTY} G=${SIM_SOURCE_DIGEST_GENERALS_DIRTY}")
        configure_file("${SIMDIGEST_PRE_CONFIGURE_FILE}" "${SIMDIGEST_POST_CONFIGURE_FILE}" @ONLY)
    else()
        SetupSimDigestMonitoring()
    endif()
endfunction()

# And off we go...
Main()
