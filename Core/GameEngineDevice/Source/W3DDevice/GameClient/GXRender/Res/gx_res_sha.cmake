# GeneralsX @feature E · rw2-C 11/10/2026 WS-C provenance: run at BUILD time (cmake -P), not configure time, so an incremental
# rebuild after a commit prints the new sha - a configure-time define would stay at the sha the tree was configured on.
#   cmake -DREPO=<repo root> -DOUT=<header> -P gx_res_sha.cmake
execute_process(COMMAND git -C "${REPO}" rev-parse HEAD OUTPUT_VARIABLE sha OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE rc)
if(NOT rc EQUAL 0 OR sha STREQUAL "")
    set(sha "unknown")
endif()
execute_process(COMMAND git -C "${REPO}" status --porcelain --untracked-files=no OUTPUT_VARIABLE dirty OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT dirty STREQUAL "")
    set(sha "${sha}-dirty")
endif()
file(WRITE "${OUT}.tmp" "#pragma once\n#define GX_RES_BUILD_SHA \"${sha}\"\n")
execute_process(COMMAND ${CMAKE_COMMAND} -E copy_if_different "${OUT}.tmp" "${OUT}")
file(REMOVE "${OUT}.tmp")
