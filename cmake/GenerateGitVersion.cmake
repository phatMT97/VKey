# VKey - Generate Git version header for Logger and CrashLog
# SPDX-License-Identifier: GPL-3.0-only

if(NOT DEFINED OUTPUT)
    message(FATAL_ERROR "OUTPUT is required")
endif()

if(NOT DEFINED SOURCE_DIR)
    set(SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/..")
endif()

execute_process(
    COMMAND git rev-parse --short HEAD
    WORKING_DIRECTORY "${SOURCE_DIR}"
    OUTPUT_VARIABLE GIT_HASH
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE RES_HASH
)

if(NOT RES_HASH EQUAL 0 OR NOT GIT_HASH)
    set(GIT_HASH "unknown")
else()
    # Check for tracked changes only (--untracked-files=no / git diff --quiet HEAD)
    execute_process(
        COMMAND git diff --quiet HEAD
        WORKING_DIRECTORY "${SOURCE_DIR}"
        RESULT_VARIABLE RES_DIRTY
        ERROR_QUIET
    )
    if(NOT RES_DIRTY EQUAL 0)
        set(GIT_HASH "${GIT_HASH}-dirty")
    endif()
endif()

if(DEFINED ENV{GITHUB_RUN_NUMBER})
    set(CI_RUN "$ENV{GITHUB_RUN_NUMBER}")
else()
    set(CI_RUN "")
endif()

set(CONTENT
"// Auto-generated git version header — do not edit.
#pragma once

#define VKEY_GIT_HASH_STR  \"${GIT_HASH}\"
#define VKEY_GIT_HASH_WSTR L\"${GIT_HASH}\"
#define VKEY_CI_RUN_STR    \"${CI_RUN}\"
#define VKEY_CI_RUN_WSTR   L\"${CI_RUN}\"
")

set(CURRENT_CONTENT "")
if(EXISTS "${OUTPUT}")
    file(READ "${OUTPUT}" CURRENT_CONTENT)
endif()

# Only rewrite if content actually changed to avoid triggering unnecessary rebuilds
if(NOT "${CURRENT_CONTENT}" STREQUAL "${CONTENT}")
    get_filename_component(OUT_DIR "${OUTPUT}" DIRECTORY)
    file(MAKE_DIRECTORY "${OUT_DIR}")
    file(WRITE "${OUTPUT}" "${CONTENT}")
endif()
