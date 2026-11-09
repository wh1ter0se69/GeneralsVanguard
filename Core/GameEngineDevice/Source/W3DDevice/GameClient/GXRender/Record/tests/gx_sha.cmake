# GeneralsX @test E · rw2-F1 11/10/2026: writes OUT = #define GX_RECORD_BUILD_SHA "<HEAD sha>[+dirty|+archive]" for SRC's checkout,
# rewriting the file only when the text changes (so an unchanged tree does not relink).
execute_process(COMMAND git rev-parse HEAD WORKING_DIRECTORY "${SRC}" OUTPUT_VARIABLE sha OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE rc ERROR_QUIET)
if(NOT rc EQUAL 0)
    # a git archive has no .git: the driver that made the archive passes its full sha in GX_BUILD_SHA
    if(DEFINED ENV{GX_BUILD_SHA})
        set(sha "$ENV{GX_BUILD_SHA}+archive")
    else()
        set(sha "unknown")
    endif()
endif()
execute_process(COMMAND git status --porcelain --untracked-files=no -- . WORKING_DIRECTORY "${SRC}/.."
    OUTPUT_VARIABLE dirty OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(dirty)
    set(sha "${sha}+dirty")
endif()
set(text "#define GX_RECORD_BUILD_SHA \"${sha}\"\n")
if(EXISTS "${OUT}")
    file(READ "${OUT}" old)
endif()
if(NOT "${old}" STREQUAL "${text}")
    file(WRITE "${OUT}" "${text}")
endif()
