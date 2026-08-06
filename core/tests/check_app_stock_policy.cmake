# S5LBox -- the host iOS app must remain a stock-device application.
#
# A jailbroken test phone provides SSH, trust-cache access and standalone CLI
# profiling. None of that is a product dependency. This deliberately permits
# the app's "Jailbreak" setting because it modifies the emulated iPhone OS 3
# guest; it rejects only host jailbreak/private-signing contracts.

if(NOT DEFINED APP_DIR OR NOT IS_DIRECTORY "${APP_DIR}")
    message(FATAL_ERROR
        "check_app_stock_policy.cmake requires -DAPP_DIR=<app directory>")
endif()

set(entitlements "${APP_DIR}/S5LBox.entitlements")
set(project "${APP_DIR}/project.yml")
set(info_plist "${APP_DIR}/Info.plist")
foreach(required "${entitlements}" "${project}" "${info_plist}")
    if(NOT EXISTS "${required}")
        message(FATAL_ERROR "stock-host policy input is missing: ${required}")
    endif()
endforeach()

file(READ "${entitlements}" entitlement_text)
if(entitlement_text MATCHES "<key>[^<]+</key>")
    message(FATAL_ERROR
        "${entitlements} is not empty; the shipping app must request no "
        "private, JIT, debug or increased-memory entitlement")
endif()

file(GLOB_RECURSE source_files LIST_DIRECTORIES false
    "${APP_DIR}/Sources/*.c"
    "${APP_DIR}/Sources/*.h"
    "${APP_DIR}/Sources/*.m"
    "${APP_DIR}/Sources/*.mm")
list(LENGTH source_files source_count)
if(source_count LESS 1)
    message(FATAL_ERROR "no app source files found under ${APP_DIR}/Sources")
endif()

set(checked_files
    "${project}"
    "${entitlements}"
    "${info_plist}"
    ${source_files})
set(forbidden_markers
    "/var/jb"
    "jbctl"
    "dopamine"
    "dynamic-codesigning"
    "get-task-allow"
    "com.apple.developer.kernel.increased-memory-limit"
    "platform-application"
    "com.apple.private.security.no-container"
    "com.apple.private.skip-library-validation"
    "mobilesubstrate"
    "libhooker"
    "ellekit")

set(violations "")
foreach(path IN LISTS checked_files)
    file(READ "${path}" contents)
    string(TOLOWER "${contents}" contents_lower)
    foreach(marker IN LISTS forbidden_markers)
        string(FIND "${contents_lower}" "${marker}" found)
        if(NOT found EQUAL -1)
            list(APPEND violations "  ${path}: ${marker}")
        endif()
    endforeach()
endforeach()

if(violations)
    string(REPLACE ";" "\n" pretty "${violations}")
    message(FATAL_ERROR
        "jailbreak/private host dependency leaked into the iOS app:\n${pretty}")
endif()

message(STATUS
    "stock-host policy: empty entitlements, no jailbreak-only dependency")
