# S5LBox -- pin the measured iOS execution-engine policy.
#
# The product selects the callback-free live-byte tier after balanced 10M and
# 100M A9 replays proved complete guest equality and a sustained 6.06% median
# win. The decoded graph remains rejected after its exact 6.17x regression.
# The separate direct-write contract is also required: disabling it measured
# 4.14% slower with identical final evidence.
#
# This checks the XcodeGen input rather than generated project output. Public CI
# can therefore defend the shipping decision without Xcode or Apple firmware.

if(NOT DEFINED APP_PROJECT)
    message(FATAL_ERROR
        "check_app_execution_policy.cmake requires -DAPP_PROJECT=<path>")
endif()
if(NOT EXISTS "${APP_PROJECT}")
    message(FATAL_ERROR "no such app project definition: ${APP_PROJECT}")
endif()

file(READ "${APP_PROJECT}" text)

foreach(required S5LBOX_STATIC_A64_ENGINE S5LBOX_STATIC_A64_NATIVE)
    string(REGEX MATCHALL "${required}" occurrences "${text}")
    list(LENGTH occurrences count)
    if(NOT count EQUAL 1 OR NOT text MATCHES "${required}=1")
        message(FATAL_ERROR
            "${APP_PROJECT} must compile ${required}=1 exactly once; found "
            "${count} references")
    endif()
endforeach()

string(REGEX MATCHALL "S5LBOX_STATIC_A64_DEFAULT_COMPACT_RAW"
    compact_occurrences "${text}")
list(LENGTH compact_occurrences compact_count)
if(NOT compact_count EQUAL 1 OR
   NOT text MATCHES "S5LBOX_STATIC_A64_DEFAULT_COMPACT_RAW=1")
    message(FATAL_ERROR
        "${APP_PROJECT} must select the physically accepted compact live-byte "
        "policy exactly once; found ${compact_count} references")
endif()

string(REGEX MATCHALL "S5LBOX_STATIC_A64_DEFAULT_DIRECT_WRITES"
    direct_occurrences "${text}")
list(LENGTH direct_occurrences direct_count)
if(NOT direct_count EQUAL 1 OR
   NOT text MATCHES "S5LBOX_STATIC_A64_DEFAULT_DIRECT_WRITES=1")
    message(FATAL_ERROR
        "${APP_PROJECT} must select the measured direct-write contract "
        "exactly once; found ${direct_count} references")
endif()

if(text MATCHES "S5LBOX_STATIC_A64_DEFAULT_GRAPH")
    message(FATAL_ERROR
        "${APP_PROJECT} selects the signed graph; it may not become the "
        "shipping default until a physical-device full-guest replay proves "
        "an end-to-end win")
endif()

message(STATUS
    "iOS policy: compact live-byte engine plus direct writes selected; "
    "rejected graph disabled")
