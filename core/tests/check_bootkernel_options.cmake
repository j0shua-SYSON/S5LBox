# S5LBox -- boot option resolution tests.
#
# The option layer's whole claim is that a command line resolves to exactly one
# configuration and that the run says which. That claim is only worth anything
# if it is checked, so these cases pin the resolver itself: the defaults, the
# --no- form beating a default, a historical short flag resolving to the same
# row as its long name, contradictions and incoherent pairs failing closed, and
# an unknown option still being refused.
#
# --print-config is the observation point. It resolves and validates the whole
# command line and then exits 0 WITHOUT opening the kernel, the device tree, the
# rootfs or a work image, so every case below runs against non-existent paths
# and touches no filesystem -- which is what keeps it runnable in public CI,
# where Apple's firmware cannot exist.
#
# Copyright (c) 2026 j0shua-SYSON. MIT licensed.

if(NOT DEFINED BOOTKERNEL OR NOT EXISTS "${BOOTKERNEL}")
    message(FATAL_ERROR "BOOTKERNEL must name the built bootkernel executable")
endif()

# Run and require success plus a literal substring. `expected` may carry several
# substrings separated by "|", all of which must be present -- a resolved
# configuration is a set of simultaneous facts and checking one at a time would
# let a mutation that breaks a neighbouring row slip through.
function(expect_config name expected)
    execute_process(
        COMMAND "${BOOTKERNEL}" ${ARGN}
        RESULT_VARIABLE result
        OUTPUT_VARIABLE stdout
        ERROR_VARIABLE stderr)
    set(combined "${stdout}${stderr}")
    if(NOT result EQUAL 0)
        message(FATAL_ERROR
            "${name}: expected exit 0, got ${result}:\n${combined}")
    endif()
    string(REPLACE "|" ";" wanted "${expected}")
    foreach(want IN LISTS wanted)
        string(FIND "${combined}" "${want}" found)
        if(found EQUAL -1)
            message(FATAL_ERROR
                "${name}: expected '${want}' in the resolved config, got:\n${combined}")
        endif()
    endforeach()
endfunction()

function(expect_refused name expected)
    execute_process(
        COMMAND "${BOOTKERNEL}" ${ARGN}
        RESULT_VARIABLE result
        OUTPUT_VARIABLE stdout
        ERROR_VARIABLE stderr)
    set(combined "${stdout}${stderr}")
    if(result EQUAL 0)
        message(FATAL_ERROR "${name}: invalid invocation returned success")
    endif()
    string(FIND "${combined}" "${expected}" found)
    if(found EQUAL -1)
        message(FATAL_ERROR
            "${name}: expected diagnostic '${expected}', got:\n${combined}")
    endif()
endfunction()

# --------------------------------------------------------------------------
# 1. Default resolution, and --print-config exiting 0 without firmware.
#
# These are the values every run in docs/BOOTLOG.md is read against. A change
# to any of them silently changes the meaning of ~60 recorded runs, so they are
# spelled out here rather than left implicit in the table.
expect_config(defaults_resolve
    "mbx=0|sha1=0|baseband=0|spi2=0|usb-otg=0|framebuffer=0|iomfb-display=0|vram=1|lcd-panel-id=1|memory-reg=1|rtc-patch=1|fstab-fixup=1|ca-software-render=0|activate=1|jb-codesign=0|jb-payload=0|ppp=0|ramdisk-low=0|stop-on-abort=0|kext-map=0|call-probe-regs=1"
    absent-kernel --print-config)

# Nothing but --print-config itself came from the command line.
expect_config(defaults_are_marked_as_defaults
    "config-cli : print-config"
    absent-kernel --print-config)

# --activate defaults on and is not implemented, so it must say so, visibly,
# every single run. A silent no-op here is the specific failure being designed
# against.
expect_config(activation_is_never_silent
    "activation : requested but NOT APPLIED"
    absent-kernel --print-config)
expect_config(activation_off_is_also_stated
    "activation : disabled by --no-activate"
    absent-kernel --no-activate --print-config)

# --jailbreak defaults OFF, so a default run says nothing about it; asking for
# it must produce an explicit not-applied line for each half rather than a run
# that quietly behaves identically.
expect_config(jailbreak_default_off
    "jb-codesign=0|jb-payload=0"
    absent-kernel --print-config)
# The code-signing half is APPLIED as of the boot-args/device-tree wiring, and
# this case pins both halves of what that means: that it is applied, and that
# the header still refuses to claim it has been demonstrated. Booting clean
# with enforcement disabled is not evidence that enforcement is disabled --
# nothing unsigned has executed -- and a run header that dropped the second
# sentence would read as proof of something never measured.
expect_config(jailbreak_codesign_half_alone
    "jb-codesign=1|jb-payload=0|jailbreak  : code-signing half APPLIED|applied, NOT demonstrated"
    absent-kernel --jb-codesign --print-config)

# --ca-software-render defaults ON as of 2026-07-29, because this VM un-matches
# /arm-io/mbx and without the override CA::WindowServer takes MBX2D, whose
# global context is NULL for exactly that reason. run149 and run150 differ in
# this flag alone, at the same budget, and draw 273206 bytes against 1890.
#
# It is written into a work image, so with no --external-md there is nowhere to
# put it and the default resolves to off -- SILENTLY, because this layer's rule
# is that a default is never grounds for a rejection. The defaults_resolve case
# above pins that half (it asks for no work image and expects
# ca-software-render=0); these pin the rest.
expect_config(software_render_on_by_default_with_a_work_image
    "ca-software-render=1"
    absent-kernel -d absent-tree --external-md absent-source new-work --print-config)
# Un-asked-for and inapplicable resolves to off instead of refusing. Each of
# these would be an ERROR if it had been requested by name -- see the three
# expect_refused cases below, which still pass exactly those command lines.
expect_config(software_render_default_yields_to_a_matched_gpu
    "mbx=1|ca-software-render=0"
    absent-kernel -d absent-tree --external-md absent-source new-work --mbx --print-config)
expect_config(software_render_default_yields_to_no_vram
    "vram=0|ca-software-render=0"
    absent-kernel -F -d absent-tree --external-md absent-source new-work --no-vram --print-config)
# And it is still a DEFAULT, not something the command line asked for: the
# resolution moves the value, never the provenance.
expect_config(software_render_default_is_not_claimed_as_cli
    "config-cli : print-config"
    absent-kernel -d absent-tree --external-md absent-source new-work --print-config)

# --------------------------------------------------------------------------
# 2. --no- overrides a default.
expect_config(no_form_overrides_default
    "vram=0|config-cli : framebuffer no-vram"
    absent-kernel -F --no-vram --print-config)
expect_config(no_form_overrides_default_on_patch
    "memory-reg=0"
    absent-kernel --no-memory-reg --print-config)
expect_config(no_form_overrides_default_activate
    "activate=0"
    absent-kernel --no-activate --print-config)

# --------------------------------------------------------------------------
# 3. Historical short flags resolve to exactly their long form.
#
# This is the non-negotiable one: every command line in docs/BOOTLOG.md, in
# tools/run23-cold-replay.ps1 and in the work/ launchers is written in short
# flags, and all of them must keep meaning what they meant.
expect_config(short_u_is_usb_otg
    "usb-otg=1" absent-kernel -u --print-config)
expect_config(long_usb_otg_matches_short
    "usb-otg=1" absent-kernel --usb-otg --print-config)
expect_config(short_S_is_sha1
    "sha1=1" absent-kernel -S --print-config)
expect_config(short_F_is_framebuffer
    "framebuffer=1|iomfb-display=0" absent-kernel -F --print-config)
# The changed-frame meter observes the live CLCD surface and has no honest
# answer without one. It is host-only (like --fast), so the relaunch line is
# its provenance rather than the emulated-machine toggle table.
expect_config(frame_meter_requires_and_accepts_framebuffer
    "framebuffer=1|--frame-meter"
    absent-kernel -F --frame-meter --print-config)
expect_refused(frame_meter_without_framebuffer
    "--frame-meter requires --framebuffer"
    absent-kernel --frame-meter --print-config)
# The app-facing throughput path is host-only too. NAT defaults on but is inert
# while PPP is off, so the ordinary configuration must remain accepted; the
# first live attempt caught an over-broad gate that rejected this exact case.
expect_config(run_api_accepts_the_default_inert_nat
    "nat=1|ppp=0|--run-api"
    absent-kernel --run-api --print-config)
# Checkpoints do not need an instruction callback: the runner shortens its next
# 100000-instruction chunk to land on the exact absolute boundary.
expect_config(run_api_accepts_exact_checkpoints
    "snapshot-at count    1|--run-api"
    absent-kernel --run-api -n 100 --snapshot-at 100 absent-snapshot
    --print-config)
# Frame publication is chunk-boundary work in the app too, so it is the one
# observer that can coexist with this path without losing its contract.
expect_config(run_api_accepts_frame_meter
    "framebuffer=1|--run-api|--frame-meter"
    absent-kernel -F --run-api --frame-meter --print-config)
# The physical-device A/B must use one executable and change only the runtime
# engine policy. Its provenance is the relaunch line, like the other host-only
# controls, and accepting it without --run-api would make it look like a normal
# boot mode rather than the bounded performance control it is.
expect_config(interpreter_control_is_explicit
    "--run-api|--interpreter-control"
    absent-kernel --run-api --interpreter-control --print-config)
expect_refused(interpreter_control_requires_run_api
    "--interpreter-control requires --run-api"
    absent-kernel --interpreter-control --print-config)
# The compact live-byte engine is selected only inside the same bounded harness
# and is mutually exclusive with the literal interpreter control. This keeps a
# physical A9 A/B in one executable without turning the experiment into a boot
# default or letting an impossible hybrid policy reach machine initialization.
expect_config(compact_raw_control_is_explicit
    "--run-api|--compact-raw-control"
    absent-kernel --run-api --compact-raw-control --print-config)
expect_refused(compact_raw_control_requires_run_api
    "--compact-raw-control requires --run-api"
    absent-kernel --compact-raw-control --print-config)
expect_refused(compact_raw_and_interpreter_are_exclusive
    "mutually exclusive execution policies"
    absent-kernel --run-api --compact-raw-control --interpreter-control
    --print-config)
expect_refused(compact_raw_refuses_pre_step_hle
    "cannot be combined with --hle"
    absent-kernel --run-api --compact-raw-control --hle --print-config)
# The first physical A9 replay proved that the diagnostic observer's revoked
# write consent was not app-identical. These controls must remain explicit,
# bounded and independently switchable in one executable.
expect_config(canonical_bus_control_is_explicit
    "--run-api|--canonical-bus"
    absent-kernel --run-api --canonical-bus --print-config)
expect_refused(canonical_bus_control_requires_run_api
    "--canonical-bus requires --run-api"
    absent-kernel --canonical-bus --print-config)
expect_config(direct_write_off_control_is_explicit
    "--run-api|--canonical-bus|--no-direct-ram-writes"
    absent-kernel --run-api --canonical-bus --no-direct-ram-writes
    --print-config)
expect_refused(direct_write_off_control_requires_canonical_bus
    "--no-direct-ram-writes requires --canonical-bus"
    absent-kernel --run-api --no-direct-ram-writes --print-config)
# The app drains its touch queue between s5l8900_run() chunks. The harness can
# therefore split a bounded chunk at an absolute count and inject a tap at the
# same ownership boundary without installing a per-instruction observer.
expect_config(run_api_accepts_scheduled_tap
    "touch taps           1|--run-api|--touch"
    absent-kernel --run-api --touch 1000:160:240 --print-config)
# Multi-report gestures and buttons still use the literal runner's
# per-instruction retry scheduler. Refuse those rather than accepting a request
# the app-shaped path cannot currently execute faithfully.
expect_refused(run_api_refuses_scheduled_gesture
    "--run-api cannot be combined"
    absent-kernel --run-api --drag 1000:10:240:300:240 --print-config)
# Replacement HLE now uses the same bounded exact-PC hook as the experimental
# iOS build, so this combination is the faithful way to test that product path.
expect_config(run_api_accepts_replacement_hle
    "hle=1|--run-api"
    absent-kernel --run-api --hle --print-config)
# The differential verifier must observe the complete original function and
# its return boundary, which remains a literal-runner diagnostic.
expect_refused(run_api_refuses_hle_verify
    "--run-api cannot be combined"
    absent-kernel --run-api --hle-verify --print-config)
# Full sequence profiling requires the literal instruction boundary. It is a
# host observer, so the relaunch line (not the machine-toggle table) records it.
expect_config(sequence_profile_is_explicit
    "--sequence-profile"
    absent-kernel --sequence-profile --print-config)
expect_refused(sequence_profile_refuses_run_api
    "--sequence-profile requires the literal runner"
    absent-kernel --sequence-profile --run-api --print-config)
expect_refused(sequence_profile_refuses_frame_meter
    "--sequence-profile requires the literal runner"
    absent-kernel -F --sequence-profile --frame-meter --print-config)
# -g was never a pure --mbx: it has always implied -F and v_display=1 as well.
expect_config(short_g_is_the_whole_graphics_experiment
    "framebuffer=1|iomfb-display=1|mbx=1"
    absent-kernel -g --print-config)
# -B has always covered both baseband nubs at once.
expect_config(short_B_covers_both_baseband_nubs
    "baseband=1|spi2=1" absent-kernel -B --print-config)
# ...and the split halves are now reachable on their own.
expect_config(spi2_alone
    "baseband=0|spi2=1" absent-kernel --spi2 --print-config)
# -K and -M disable; their long forms are the positive names.
expect_config(short_K_disables_rtc_patch
    "rtc-patch=0" absent-kernel -K --print-config)
expect_config(short_M_disables_memory_reg
    "memory-reg=0" absent-kernel -M --print-config)
expect_config(keep_fstab_disables_fstab_fixup
    "fstab-fixup=0" absent-kernel --keep-fstab --print-config)
expect_config(short_a_is_stop_on_abort
    "stop-on-abort=1" absent-kernel -a --print-config)
expect_config(short_Y_is_ramdisk_low
    "ramdisk-low=1" absent-kernel -Y --print-config)

# --------------------------------------------------------------------------
# 4. Unknown options are still refused. The parser fails closed and must keep
#    doing so, including for a plausible-looking --no- spelling of a row that
#    does not exist.
expect_refused(unknown_option_still_refused "unknown option"
    absent-kernel --not-a-real-option --print-config)
expect_refused(unknown_no_option_refused "unknown option"
    absent-kernel --no-such-feature --print-config)
expect_refused(missing_value_still_refused "missing option value"
    absent-kernel --jailbreak-payload)

# --------------------------------------------------------------------------
# 5. Incoherent combinations fail closed, before any firmware is opened.
expect_refused(contradictory_toggle "cannot be both enabled and disabled"
    absent-kernel --vram --no-vram --print-config)
expect_refused(contradictory_through_compound "cannot be both enabled and disabled"
    absent-kernel -g --no-mbx --print-config)
expect_refused(no_vram_without_framebuffer "only means anything with --framebuffer"
    absent-kernel --no-vram --print-config)
expect_refused(no_panel_without_framebuffer "only means anything with --framebuffer"
    absent-kernel --no-lcd-panel-id --print-config)
expect_refused(iomfb_without_framebuffer "--iomfb-display requires --framebuffer"
    absent-kernel --iomfb-display --print-config)
expect_refused(print_config_with_kext_map "cannot be combined with --kext-map"
    absent-kernel -L --print-config)
expect_refused(payload_without_jailbreak "but --jb-payload is off"
    absent-kernel --jailbreak-payload absent-payload --print-config)
expect_refused(jailbreak_without_external_md "requires --external-md"
    absent-kernel --jailbreak --print-config)
# --ppp writes a launchd job into the work image, so it needs the one mode that
# has one. Without this gate a run would append uart4_dma_enable=0, arm the LCP
# scan, and report a configuration whose guest half never happened -- which
# reads exactly like "pppd ran and sent nothing".
expect_refused(ppp_without_external_md "--ppp requires --external-md"
    absent-kernel --ppp --print-config)
expect_config(ppp_off_by_default
    "ppp=0" absent-kernel --print-config)
expect_config(ppp_resolves_under_external_md
    "ppp=1|config-cli : ppp"
    absent-kernel -d absent-tree --external-md absent-source new-work
    --ppp --print-config)
# The payload path is validated in preflight like every other input, so a typo
# costs a second rather than a boot.
expect_refused(unreadable_payload "--jailbreak-payload: cannot read"
    absent-kernel -d absent-tree --external-md absent-source new-work
    --jailbreak --jailbreak-payload absent-payload --print-config)
# --mbx and --ca-software-render describe two different machines: the renderer
# override exists precisely because the GPU is un-matched.
expect_refused(mbx_with_software_render "--mbx conflicts with --ca-software-render"
    absent-kernel -d absent-tree --external-md absent-source new-work
    --mbx --ca-software-render --print-config)
# --no-vram deliberately restores a render failure; --ca-software-render tries
# to work around one.
expect_refused(no_vram_with_software_render "--no-vram conflicts with --ca-software-render"
    absent-kernel -d absent-tree --external-md absent-source new-work
    -F --no-vram --ca-software-render --print-config)
# The external-md kernel gate is one all-or-nothing transaction, so the RTC
# patch cannot be dropped from it.
expect_refused(no_rtc_patch_under_external_md "--no-rtc-patch"
    absent-kernel -d absent-tree --external-md absent-source new-work
    --no-rtc-patch --print-config)

# --------------------------------------------------------------------------
# 6. A rejected command line stays rejected even though --print-config would
#    otherwise exit 0: validation runs first, so --print-config can never be
#    used to make an incoherent run look acceptable.
expect_refused(print_config_does_not_excuse_nonsense "requires --external-md"
    absent-kernel --ca-software-render --print-config)

# --------------------------------------------------------------------------
# 7. --touch, the scheduled tap.
#
# Every field is validated at parse time rather than at injection time, and
# that is the whole point of these cases: a tap whose coordinate is off the
# panel would otherwise be refused by the device on every instruction from
# <at> to the end of the run and read exactly like "the guest never drained a
# report" -- a wrong answer to the only question the run was asked.
expect_config(touch_is_counted
    "touch taps           1" absent-kernel --touch 1000:160:240 --print-config)
expect_config(touch_repeats
    "touch taps           3" absent-kernel --touch 1000:0:0 --touch 2000:319:479
    --touch 3000:1:2:500 --print-config)
expect_config(no_touch_is_zero
    "touch taps           0" absent-kernel --print-config)
expect_refused(touch_off_panel_x "off a 320x480 panel"
    absent-kernel --touch 1000:320:240 --print-config)
expect_refused(touch_off_panel_y "off a 320x480 panel"
    absent-kernel --touch 1000:160:480 --print-config)
expect_refused(touch_zero_hold "hold must not be zero"
    absent-kernel --touch 1000:160:240:0 --print-config)
expect_refused(touch_malformed "expected <at>:<x>:<y>"
    absent-kernel --touch 1000-160-240 --print-config)
expect_refused(touch_missing_field "expected <at>:<x>:<y>"
    absent-kernel --touch 1000:160 --print-config)
expect_refused(touch_trailing_junk "expected <at>:<x>:<y>"
    absent-kernel --touch 1000:160:240:500x --print-config)
expect_refused(touch_missing_value "missing option value"
    absent-kernel --touch)

# --------------------------------------------------------------------------
# 7a. --button, the scheduled press on one of the board's five switches.
#
# The same shape as --touch's checks, plus the one that is specific to a
# button: a hold shorter than the guest's own 14 ms debounce is refused, because
# AppleM68Buttons samples a pin only after a timer it arms from the interrupt,
# so a press that has already been undone by then is reported as nothing at all.
# Refusing it at parse time is what stops a run spending an hour proving that.
expect_config(button_is_counted
    "button presses       1" absent-kernel --button menu:1000 --print-config)
expect_config(button_repeats
    "button presses       3" absent-kernel --button menu:1000
    --button hold:2000 --button ringerab:3000:6000000 --print-config)
expect_config(no_button_is_zero
    "button presses       0" absent-kernel --print-config)
# Every one of the tree's five names is accepted, and nothing else is.
expect_config(button_all_five_names
    "button presses       5" absent-kernel --button hold:1 --button menu:2
    --button volup:3 --button voldown:4 --button ringerab:5 --print-config)
expect_refused(button_unknown_name "unknown button 'home'"
    absent-kernel --button home:1000 --print-config)
expect_refused(button_empty_name "unknown button ''"
    absent-kernel --button :1000 --print-config)
expect_refused(button_short_hold "below the guest's own debounce"
    absent-kernel --button menu:1000:1000 --print-config)
expect_refused(button_zero_hold "below the guest's own debounce"
    absent-kernel --button menu:1000:0 --print-config)
expect_refused(button_malformed "expected <name>:<at>"
    absent-kernel --button menu-1000 --print-config)
expect_refused(button_missing_at "expected <name>:<at>"
    absent-kernel --button menu: --print-config)
expect_refused(button_trailing_junk "expected <name>:<at>"
    absent-kernel --button menu:1000:24000000x --print-config)
expect_refused(button_missing_value "missing option value"
    absent-kernel --button)

# --------------------------------------------------------------------------
# 7b. --drag, the scheduled gesture.
#
# The same parse-time discipline as --touch's, for a stronger version of the
# same reason: an invalid drag would be refused by the device on every
# instruction from <at> to the end of the run, which reads exactly like "the
# guest never drained a report" -- and a drag is many more instructions of that
# than a tap is.
#
# ONE SLOT PER GESTURE is pinned here too. run90 faked a slider drag out of
# eight --touch points and spent every available slot doing it; a drag that
# still cost one slot per point would not have fixed anything.
expect_config(drag_is_counted
    "drag gestures        1"
    absent-kernel --drag 1000:10:240:300:240 --print-config)
expect_config(drag_expands_internally_into_one_slot
    "drag gestures        8|touch taps           0"
    absent-kernel --drag 1:0:0:319:479:64 --drag 2:0:0:1:1
    --drag 3:0:0:1:1 --drag 4:0:0:1:1 --drag 5:0:0:1:1 --drag 6:0:0:1:1
    --drag 7:0:0:1:1 --drag 8:0:0:1:1 --print-config)
expect_config(no_drag_is_zero
    "drag gestures        0" absent-kernel --print-config)
# --touch is untouched by any of this: runs 77-79 and run90 are read against
# its behaviour and both counters must stay independent.
expect_config(drag_and_touch_are_separate_pools
    "touch taps           1|drag gestures        1"
    absent-kernel --touch 1000:160:240 --drag 2000:10:240:300:240
    --print-config)
expect_refused(drag_ninth_is_refused "at most 8 drags"
    absent-kernel --drag 1:0:0:1:1 --drag 2:0:0:1:1 --drag 3:0:0:1:1
    --drag 4:0:0:1:1 --drag 5:0:0:1:1 --drag 6:0:0:1:1 --drag 7:0:0:1:1
    --drag 8:0:0:1:1 --drag 9:0:0:1:1 --print-config)
# Every coordinate of both endpoints is bounded, not just the start.
expect_refused(drag_start_off_panel_x "leaves a 320x480 panel"
    absent-kernel --drag 1000:320:240:10:240 --print-config)
expect_refused(drag_start_off_panel_y "leaves a 320x480 panel"
    absent-kernel --drag 1000:10:480:10:240 --print-config)
expect_refused(drag_end_off_panel_x "leaves a 320x480 panel"
    absent-kernel --drag 1000:10:240:320:240 --print-config)
expect_refused(drag_end_off_panel_y "leaves a 320x480 panel"
    absent-kernel --drag 1000:10:240:10:480 --print-config)
# Zero intermediate reports is not a small drag. It is MakeTouch followed by
# BreakTouch -- a tap -- and run90 measured what happens to a sequence of those:
# a 16/17/9/2 funnel, 16 reports enqueued by the kernel and 2 events delivered.
expect_refused(drag_without_movement_reports "steps must be at least 1"
    absent-kernel --drag 1000:10:240:300:240:0 --print-config)
expect_refused(drag_too_many_steps "at most 64 steps"
    absent-kernel --drag 1000:10:240:300:240:65 --print-config)
# A span that cannot give every report its own instruction can never complete:
# the device holds exactly one report, so two due at the same instruction are
# not both acceptable however long the harness retries.
expect_refused(drag_zero_span "cannot pace"
    absent-kernel --drag 1000:10:240:300:240:8:0 --print-config)
expect_refused(drag_span_one_short "cannot pace"
    absent-kernel --drag 1000:10:240:300:240:8:8 --print-config)
# ...and exactly one instruction per gap is the boundary, which is accepted.
expect_config(drag_minimum_legal_span
    "drag gestures        1"
    absent-kernel --drag 1000:10:240:300:240:8:9 --print-config)
expect_refused(drag_malformed "expected <at>:<x0>:<y0>:<x1>:<y1>"
    absent-kernel --drag 1000-10-240-300-240 --print-config)
expect_refused(drag_missing_field "expected <at>:<x0>:<y0>:<x1>:<y1>"
    absent-kernel --drag 1000:10:240:300 --print-config)
expect_refused(drag_trailing_junk "expected <at>:<x0>:<y0>:<x1>:<y1>"
    absent-kernel --drag 1000:10:240:300:240:8:59328000x --print-config)
expect_refused(drag_trailing_junk_on_steps "expected <at>:<x0>:<y0>:<x1>:<y1>"
    absent-kernel --drag 1000:10:240:300:240:8x --print-config)
expect_refused(drag_missing_value "missing option value"
    absent-kernel --drag)

# --------------------------------------------------------------------------
# 8. --call-probe / --call-probe-kernel, and the register-file line.
#
# The two flags share one PC namespace and one ring, so the count is the thing
# to pin: a second flag that quietly resolved into its own pool would let a run
# arm nine probes while the header said eight.
expect_config(call_probe_is_counted
    "call probes          1" absent-kernel --call-probe 0x33cfdee0 --print-config)
expect_config(call_probe_kernel_shares_the_pool
    "call probes          2"
    absent-kernel --call-probe 0x33cfdee0 --call-probe-kernel 0xc0526a4c
    --print-config)
expect_config(no_call_probe_is_zero
    "call probes          0" absent-kernel --print-config)
# A Thumb entry is named by its even VA, so 0x33cfdee1 and 0x33cfdee0 are the
# same site and the second is a duplicate rather than a ninth probe.
expect_refused(call_probe_duplicate_pc "given twice"
    absent-kernel --call-probe 0x33cfdee0 --call-probe 0x33cfdee1 --print-config)
expect_refused(call_probe_zero_pc "pc must not be zero"
    absent-kernel --call-probe 0 --print-config)
expect_refused(call_probe_missing_value "missing option value"
    absent-kernel --call-probe)

# The register-file line defaults ON. That default is what makes a probed boot
# yield r4-r12 without anybody having remembered to ask, which matters because
# a register that was not printed cannot be recovered without paying for the
# boot again -- and it is the reason MultitouchHID.plugin's dyld-chosen load
# base is reachable at all, since it exists only as r12 at one instruction.
expect_config(call_probe_regs_default_on
    "call-probe-regs=1"
    absent-kernel --call-probe 0x33cfdee0 --print-config)
# ...and nothing on a default command line asked for it, so it must not appear
# on the config-cli line. This is the row that would catch a default flipped by
# accident: config: says what is effective, config-cli says what was requested.
expect_config(call_probe_regs_is_a_default_not_a_request
    "config-cli : print-config"
    absent-kernel --call-probe 0x33cfdee0 --print-config)
expect_config(call_probe_regs_can_be_suppressed
    "call-probe-regs=0|config-cli : print-config no-call-probe-regs"
    absent-kernel --call-probe 0x33cfdee0 --no-call-probe-regs --print-config)
# Both directions are refused with no probe armed, for the reason --no-vram is
# refused without --framebuffer: with nothing to capture there is no report for
# either spelling to change, and an unhonourable request must not resolve like
# a satisfied one.
expect_refused(call_probe_regs_without_a_probe
    "only means anything with --call-probe"
    absent-kernel --call-probe-regs --print-config)
expect_refused(no_call_probe_regs_without_a_probe
    "only means anything with --call-probe"
    absent-kernel --no-call-probe-regs --print-config)
# The kernel-mode spelling arms it just as well.
expect_config(call_probe_regs_under_the_kernel_flag
    "call-probe-regs=0"
    absent-kernel --call-probe-kernel 0xc0526a4c --no-call-probe-regs
    --print-config)
# --call-probe-regs is its own token and must never be eaten as --call-probe's
# value: that would silently arm a probe at a nonsense PC and consume the flag.
expect_refused(call_probe_regs_is_not_a_probe_value
    "negative values are not allowed"
    absent-kernel --call-probe --call-probe-regs --print-config)
