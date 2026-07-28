# S5LBox -- the app's Info.plist is well-formed enough to be a plist.
#
# WHY THIS EXISTS. app/Info.plist carried an illegal XML comment for an unknown
# length of time: XML 1.0 section 2.5 says the string "--" MUST NOT occur
# inside a comment, and one of the explanatory comments used it as a dash. No
# test read the file, and the failure mode of a malformed Info.plist is a build
# or launch error a long way from the punctuation that caused it.
#
# WHAT IT CHECKS, and it is deliberately narrow: the structural rules that a
# hand-edited, heavily-commented plist actually gets wrong. It is not an XML
# parser and does not pretend to be one -- `plutil -lint` on a macOS runner is
# the real validator, and this is the part that can run anywhere CTest does.

if(NOT DEFINED PLIST)
    message(FATAL_ERROR "check_info_plist.cmake requires -DPLIST=<path>")
endif()
if(NOT EXISTS "${PLIST}")
    message(FATAL_ERROR "no such file: ${PLIST}")
endif()

file(READ "${PLIST}" text)
string(REPLACE ";" "\\;" text "${text}")
string(REPLACE "\n" ";" lines "${text}")

set(in_comment FALSE)
set(line_no 0)
set(problems "")
set(open_comments 0)

foreach(line IN LISTS lines)
    math(EXPR line_no "${line_no}+1")
    set(rest "${line}")

    # Track comment nesting across lines, and check the body of each comment
    # line for the one sequence XML forbids inside one.
    if(in_comment)
        if(rest MATCHES "-->")
            string(REGEX REPLACE "-->.*$" "" body "${rest}")
            set(in_comment FALSE)
        else()
            set(body "${rest}")
        endif()
    else()
        if(rest MATCHES "<!--")
            math(EXPR open_comments "${open_comments}+1")
            string(REGEX REPLACE "^.*<!--" "" body "${rest}")
            if(body MATCHES "-->")
                string(REGEX REPLACE "-->.*$" "" body "${body}")
            else()
                set(in_comment TRUE)
            endif()
        else()
            set(body "")
        endif()
    endif()

    if(NOT body STREQUAL "" AND body MATCHES "--")
        list(APPEND problems
             "  line ${line_no}: '--' inside a comment -- XML 1.0 2.5 forbids it")
    endif()
endforeach()

if(in_comment)
    list(APPEND problems "  a comment is opened and never closed")
endif()
if(open_comments EQUAL 0)
    list(APPEND problems "  no comments at all, which means this parser is not reading the file it thinks it is")
endif()

# The keys the app depends on, checked by presence rather than by value: a
# missing one is a silent behaviour change, and UIFileSharingEnabled in
# particular is the difference between a user being able to put an IPSW on the
# device and not.
foreach(key CFBundleDisplayName CFBundleIdentifier MinimumOSVersion
            UIFileSharingEnabled LSSupportsOpeningDocumentsInPlace)
    if(NOT text MATCHES "<key>${key}</key>")
        list(APPEND problems "  <key>${key}</key> is missing")
    endif()
endforeach()

if(problems)
    string(REPLACE ";" "\n" pretty "${problems}")
    message(FATAL_ERROR "${PLIST} is not well formed:\n${pretty}")
endif()

message(STATUS "Info.plist: comments well formed, required keys present")
