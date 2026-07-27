# S5LBox -- the app's option table against bootkernel's, mechanically.
#
# app/Sources/VMOptions.c mirrors tools/bootkernel.c's BOOT_TOGGLES by hand, and
# app/Tests/test_vmoptions.c pins that copy longhand. What neither of those can
# do is notice a row bootkernel GAINED: the app's test compares the app's table
# against the app's own expectation, so both can be updated together, agree
# perfectly, and still be missing a toggle that exists.
#
# That is not hypothetical. `ppp` was added to BOOT_TOGGLES and mirrored into
# VMOptions by hand; nothing anywhere would have failed if it had not been, and
# the settings screen would simply have been short one switch, silently, until
# somebody compared the two files by eye.
#
# So this runs BOTH sides and requires a partition:
#
#   every toggle bootkernel --print-config reports
#       is mirrored in VMOptions  XOR  declared omitted in VMOptions
#
# and neither app table may name a toggle bootkernel does not have. A new
# toggle therefore fails the build until somebody decides which it is, which is
# the whole point -- the decision is cheap, the silence is not.
#
# --print-config is the observation point for the same reason the sibling
# script uses it: it resolves the command line and exits 0 WITHOUT opening the
# kernel, the device tree or any image, so this runs in public CI where Apple's
# firmware cannot exist.
#
# Copyright (c) 2026 j0shua-SYSON. MIT licensed.

if(NOT DEFINED BOOTKERNEL OR NOT EXISTS "${BOOTKERNEL}")
    message(FATAL_ERROR "BOOTKERNEL must name the built bootkernel executable")
endif()
if(NOT DEFINED VMOPTIONS OR NOT EXISTS "${VMOPTIONS}")
    message(FATAL_ERROR "VMOPTIONS must name the built test_vmoptions executable")
endif()

# ---------------------------------------------------------------- bootkernel --
#
# --print-config lays the table out in groups, one row per line:
#
#     mbx                  off  default
#
# so a toggle is the first word of any line whose second word is on/off. The
# group headings have no second word and the prose lines are parenthesised, so
# neither can be mistaken for a row. Matching on the on/off column rather than
# on indentation is deliberate: indentation is formatting and could change
# without anybody thinking of this file.
execute_process(
    COMMAND "${BOOTKERNEL}" placeholder-kernel --print-config -d placeholder-dt
    RESULT_VARIABLE bk_result
    OUTPUT_VARIABLE bk_stdout
    ERROR_VARIABLE  bk_stderr)
if(NOT bk_result EQUAL 0)
    message(FATAL_ERROR
        "--print-config exited ${bk_result}:\n${bk_stdout}${bk_stderr}")
endif()

string(REPLACE "\n" ";" bk_lines "${bk_stdout}")
set(bootkernel_toggles "")
foreach(line IN LISTS bk_lines)
    string(STRIP "${line}" line)
    if(line MATCHES "^([A-Za-z0-9_-]+)[ \t]+(on|off)[ \t]")
        list(APPEND bootkernel_toggles "${CMAKE_MATCH_1}")
    endif()
endforeach()

list(LENGTH bootkernel_toggles bk_count)
if(bk_count LESS 15)
    message(FATAL_ERROR
        "only parsed ${bk_count} toggles out of --print-config, which is fewer "
        "than this project has ever had -- the output format changed and this "
        "check is reading it wrong. Refusing to pass on a parse that found "
        "nothing to compare.\n${bk_stdout}")
endif()

# ----------------------------------------------------------------- the app ---
execute_process(
    COMMAND "${VMOPTIONS}" --list
    RESULT_VARIABLE app_result
    OUTPUT_VARIABLE app_stdout
    ERROR_VARIABLE  app_stderr)
if(NOT app_result EQUAL 0)
    message(FATAL_ERROR
        "test_vmoptions --list exited ${app_result}:\n${app_stdout}${app_stderr}")
endif()

string(REPLACE "\n" ";" app_lines "${app_stdout}")
set(app_mirrored "")
set(app_omitted "")
foreach(line IN LISTS app_lines)
    string(STRIP "${line}" line)
    if(line MATCHES "^mirror:(.+)$")
        list(APPEND app_mirrored "${CMAKE_MATCH_1}")
    elseif(line MATCHES "^omit:(.+)$")
        list(APPEND app_omitted "${CMAKE_MATCH_1}")
    endif()
endforeach()

list(LENGTH app_mirrored mirrored_count)
if(mirrored_count LESS 1)
    message(FATAL_ERROR
        "test_vmoptions --list produced no mirrored rows; the check cannot "
        "mean anything.\n${app_stdout}")
endif()

# ------------------------------------------------------------ the partition --
set(unaccounted "")
foreach(toggle IN LISTS bootkernel_toggles)
    list(FIND app_mirrored "${toggle}" in_mirror)
    list(FIND app_omitted  "${toggle}" in_omit)
    if(in_mirror EQUAL -1 AND in_omit EQUAL -1)
        list(APPEND unaccounted "${toggle}")
    endif()
    if(NOT in_mirror EQUAL -1 AND NOT in_omit EQUAL -1)
        message(FATAL_ERROR
            "'${toggle}' is both mirrored and declared omitted in "
            "app/Sources/VMOptions.c. It cannot be both.")
    endif()
endforeach()

if(unaccounted)
    string(REPLACE ";" ", " pretty "${unaccounted}")
    message(FATAL_ERROR
        "bootkernel has ${bk_count} toggles and app/Sources/VMOptions.c "
        "accounts for neither of these: ${pretty}\n"
        "\n"
        "Add a row to VM_OPTIONS if a phone can meaningfully offer it (and to "
        "EXPECTED[] in app/Tests/test_vmoptions.c), or a row to VM_OMITTED "
        "with a reason if it cannot (and to EXPECTED_OMISSIONS[]). Either is "
        "fine. Leaving it out of both is what this check exists to stop.")
endif()

# And nothing invented. A name the app claims but bootkernel does not have
# would render a command line the harness rejects.
set(invented "")
foreach(name IN LISTS app_mirrored app_omitted)
    list(FIND bootkernel_toggles "${name}" found)
    if(found EQUAL -1)
        list(APPEND invented "${name}")
    endif()
endforeach()
if(invented)
    string(REPLACE ";" ", " pretty "${invented}")
    message(FATAL_ERROR
        "app/Sources/VMOptions.c names toggles bootkernel does not have: "
        "${pretty}\n"
        "A mirrored row with no counterpart renders a --flag the harness will "
        "refuse; an omitted one describes a decision about nothing.")
endif()

list(LENGTH app_omitted omitted_count)
message(STATUS
    "option mirror: ${bk_count} bootkernel toggles = ${mirrored_count} "
    "mirrored + ${omitted_count} deliberately omitted")
