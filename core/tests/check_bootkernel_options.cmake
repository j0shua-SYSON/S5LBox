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
    "mbx=0|sha1=0|baseband=0|spi2=0|usb-otg=0|framebuffer=0|iomfb-display=0|vram=1|lcd-panel-id=1|memory-reg=1|rtc-patch=1|fstab-fixup=1|ca-software-render=0|activate=1|jb-codesign=0|jb-payload=0|ppp=0|ramdisk-low=0|stop-on-abort=0|kext-map=0"
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
