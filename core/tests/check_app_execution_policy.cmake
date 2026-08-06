# S5LBox -- pin the measured iOS execution-engine policy.
#
# The signed-static A64 engine and generated native handlers remain part of the
# product build so they can be repaired and compared in the same binary. The
# graph is deliberately not selected by default: on the target iPhone 6s Plus,
# an exact canonical-bus 100M-instruction replay measured it at 1.125 Minsn/s
# and repeated interpreter controls at 6.952 and 6.937 Minsn/s. The separate
# direct-write contract is required: disabling it reduced the interpreter to
# 6.668 Minsn/s with identical final evidence.
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
    "iOS policy: interpreter plus direct writes selected; graph disabled")
