# Static ownership guard, not a substitute for UIKit build/device testing.
if(NOT DEFINED APP_DIR)
    message(FATAL_ERROR "APP_DIR is required")
endif()
file(READ "${APP_DIR}/Sources/VMRuntimeSettingsViewController.m" runtime)
file(READ "${APP_DIR}/Sources/VMSettingsViewController.m" app)
file(READ "${APP_DIR}/Sources/EmulatorViewController.m" emulator)
file(READ "${APP_DIR}/Sources/VMInstanceListViewController.m" machines)

foreach(forbidden "sharedSettings" "NSUserDefaults" "resetToDefaults"
        "VMFirmwareImportViewController" "VMGuestInstallViewController"
        "chooseGraphicsMode" "VMJitProbe")
    string(FIND "${runtime}" "${forbidden}" found)
    if(NOT found EQUAL -1)
        message(FATAL_ERROR "Runtime settings acquired app-wide control: ${forbidden}")
    endif()
endforeach()
foreach(required "Machine Settings" "runtimeDelegate" "snapshotsDirectory"
        "setRuntimePaused:" "setRuntimePausesInBackground:"
        "setRuntimeInlineConsole:" "setRuntimeInstructionCap:"
        "performAfterDismiss:" "UIAlertActionStyleCancel")
    string(FIND "${runtime}" "${required}" found)
    if(found EQUAL -1)
        message(FATAL_ERROR "Runtime settings lost its scoped control: ${required}")
    endif()
endforeach()
foreach(forbidden "VMGeneralRowSnapshots" "VMSnapshotListViewController")
    string(FIND "${app}" "${forbidden}" found)
    if(NOT found EQUAL -1)
        message(FATAL_ERROR "App settings acquired a machine-only row: ${forbidden}")
    endif()
endforeach()
string(FIND "${emulator}" "[[VMSettingsViewController alloc] init]" old_controller)
string(FIND "${emulator}" "[[VMRuntimeSettingsViewController alloc] init]" current_controller)
string(FIND "${machines}" "[[VMSettingsViewController alloc] init]" app_controller)
if(NOT old_controller EQUAL -1 OR current_controller EQUAL -1 OR app_controller EQUAL -1)
    message(FATAL_ERROR "Machines and emulator must use different settings controllers")
endif()
foreach(required "[_engine setInstructionCap:_sessionInstructionCap]"
        "_shuttingDown ||\n                         _sessionPausesInBackground"
        "BOOL inlineConsole = _sessionInlineConsole")
    string(FIND "${emulator}" "${required}" found)
    if(found EQUAL -1)
        message(FATAL_ERROR "Engine no longer consumes session-owned settings: ${required}")
    endif()
endforeach()
message(STATUS "App defaults and current-machine settings ownership passed")
