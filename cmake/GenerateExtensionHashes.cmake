# VKey - bind the app to the exact companion executables of its build.
# SPDX-License-Identifier: GPL-3.0-only

if(NOT DEFINED OUTPUT OR OUTPUT STREQUAL "")
    message(FATAL_ERROR "OUTPUT is required")
endif()

set(watchdog_hash "")
set(browser_hash "")
set(tsf_hash "")
if(DEFINED TSF AND NOT TSF STREQUAL "")
    file(SHA256 "${TSF}" tsf_hash)
endif()
if(DEFINED WATCHDOG AND NOT WATCHDOG STREQUAL "")
    file(SHA256 "${WATCHDOG}" watchdog_hash)
endif()
if(DEFINED BROWSER AND NOT BROWSER STREQUAL "")
    file(SHA256 "${BROWSER}" browser_hash)
elseif(DEFINED BROWSER_HASH_OVERRIDE AND NOT BROWSER_HASH_OVERRIDE STREQUAL "")
    set(browser_hash "${BROWSER_HASH_OVERRIDE}")
endif()

foreach(hash_name IN ITEMS watchdog_hash browser_hash tsf_hash RELEASE_WATCHDOG_HASH RELEASE_BROWSER_HASH)
    if(DEFINED ${hash_name} AND NOT "${${hash_name}}" STREQUAL "")
        string(LENGTH "${${hash_name}}" hash_length)
        if(NOT hash_length EQUAL 64 OR NOT "${${hash_name}}" MATCHES "^[0-9a-fA-F]+$")
            message(FATAL_ERROR "${hash_name} must be 64 hexadecimal characters")
        endif()
    endif()
endforeach()

string(TOLOWER "${watchdog_hash}" watchdog_hash)
string(TOLOWER "${browser_hash}" browser_hash)
string(TOLOWER "${tsf_hash}" tsf_hash)
string(TOLOWER "${RELEASE_WATCHDOG_HASH}" release_watchdog_hash)
string(TOLOWER "${RELEASE_BROWSER_HASH}" release_browser_hash)
get_filename_component(output_dir "${OUTPUT}" DIRECTORY)
file(MAKE_DIRECTORY "${output_dir}")
file(WRITE "${OUTPUT}"
    "// Generated from this version's companion binaries.\n"
    "#pragma once\n"
    "namespace NextKey::Security::BuildPins {\n"
    "inline constexpr char kWatchdogSha256[] = \"${watchdog_hash}\";\n"
    "inline constexpr char kBrowserHostSha256[] = \"${browser_hash}\";\n"
    "inline constexpr char kTsfSha256[] = \"${tsf_hash}\";\n"
    "inline constexpr char kReleaseWatchdogSha256[] = \"${release_watchdog_hash}\";\n"
    "inline constexpr char kReleaseBrowserHostSha256[] = \"${release_browser_hash}\";\n"
    "}\n")
