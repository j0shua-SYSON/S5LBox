# MSVC deprecates fopen, sscanf, strerror and most of the C string library in
# favour of its own _s variants (C4996), and every strict target here builds
# with /WX, which turns that into an error. The _s functions are not an option:
# this is C11 built for four toolchains, and these files must behave identically
# on all of them.
#
# Set for the whole directory rather than per target. Two CI failures in one
# evening were the same bug in different targets -- host_audio_capture's fopen,
# then VMInstancePaths' strerror and VMInstances' sscanf -- and both times the
# only target missing the definition was the one that happened to be new. A list
# would go stale the same way; this covers every target defined below,
# including ones nobody has written yet.
if(MSVC)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
endif()

# The emulator core: portable C11, zero platform dependencies.
# This same static library is what the iOS app links against.
add_library(emucore STATIC
    src/arm/arm_interp.c
    src/arm/vfp.c
    src/mmu.c
    src/snapshot.c
    src/vm_source.c
    src/vm_block.c
    src/md_bridge.c
    src/md_raw_bridge.c
    src/soc/uart.c
    src/soc/vic.c
    src/soc/timer.c
    src/soc/power.c
    src/soc/clcd.c
    src/soc/tvout.c
    src/soc/i2c.c
    src/soc/pcf50635.c
    src/soc/wm8991.c
    src/soc/i2s.c
    src/soc/spi.c
    src/soc/gpioic.c
    src/soc/buttons.c
    src/soc/mtz2.c
    src/soc/usbotg.c
    src/soc/pl080.c
    src/soc/nor.c
    src/soc/nand.c
    src/soc/storage.c
    src/soc/machine.c
    src/firmware/img3.c
    src/firmware/aes.c
    src/firmware/loader.c
    src/firmware/devicetree.c
    src/firmware/lzss.c
    src/firmware/macho.c
    src/firmware/ksyms.c
    src/boot/bringup.c
    # The host's PPP peer (docs/networking.md §10, N2). It belongs in emucore
    # rather than in a host-tool target for the reason the header states: it has
    # no platform dependency at all, not even a clock, so it is testable on any
    # runner and linkable into the iOS app unchanged.
    src/net/ppp.c
    # The host NAT (docs/networking.md §10, N0). Same argument as ppp.c and
    # then some: it has no socket in it at all -- every byte that must really
    # leave the machine leaves through net_egress_t, whose only implementation
    # is tools/net_host.c -- so it links into the iOS app unchanged and its
    # whole test suite runs on a runner with no network.
    src/net/net.c
    src/net/tcp.c
)
target_include_directories(emucore PUBLIC include)

# vfp.c uses sqrt/sqrtf/trunc and <fenv.h>'s feclearexcept/fetestexcept to sample
# the IEEE sticky flags. MinGW and macOS fold libm into libc, so this only shows
# up as an undefined-reference wall of text when linking on Linux — which is why
# it passed on the dev box and broke CI. PUBLIC so every consumer of emucore
# inherits it.
if(NOT WIN32 AND NOT APPLE)
    target_link_libraries(emucore PUBLIC m)
endif()

if(NOT MSVC)
    target_compile_options(emucore PRIVATE -Wall -Wextra)
    # VFPv2 has no fused multiply-add: VMLA rounds the product and then rounds
    # the sum. GCC's default -ffp-contract=fast is allowed to fuse those into
    # one FMA, which differs from hardware in the last bit. vfp.c also anchors
    # each rounding step behind a volatile store, so this flag is a second
    # belt rather than the only one — but a last-bit difference in a float is
    # exactly the silent wrongness this core refuses to ship.
    set_source_files_properties(src/arm/vfp.c PROPERTIES
        COMPILE_OPTIONS "-ffp-contract=off")
endif()

# --- dynamic recompiler (optional, off by default) --------------------------
# See docs/dynarec.md. The translator is a layer over the interpreter: anything
# it does not handle is left to arm_step(), so enabling this cannot change what
# the guest computes, only how fast. It is still off by default because nothing
# in the machine's run loop calls it yet.
if(S5LBOX_JIT)
    target_sources(emucore PRIVATE
        src/jit/a64_emit.c
        src/jit/jit_translate.c
        src/jit/jit_mem.c
    )
    target_compile_definitions(emucore PUBLIC S5LBOX_JIT)

    # Byte-exact encoder tests. Pure computation, so these run everywhere —
    # including the x86 dev box, which is the point.
    add_executable(test_a64_emit tests/test_a64_emit.c)
    target_link_libraries(test_a64_emit PRIVATE emucore)
    add_test(NAME a64_emitter COMMAND test_a64_emit)

    # Block-shape and emitted-code tests. Also host-independent.
    add_executable(test_jit tests/test_jit.c)
    target_link_libraries(test_jit PRIVATE emucore)
    add_test(NAME jit_translator COMMAND test_jit)

    # Boundary tests for the host-memory shim. Build jit_mem.c directly so this
    # target stays tiny and does not pull in the translator or machine core.
    add_executable(test_jit_mem tests/test_jit_mem.c src/jit/jit_mem.c)
    target_include_directories(test_jit_mem PRIVATE include)
    add_test(NAME jit_memory COMMAND test_jit_mem)

    # The only test that executes emitted code. It exits successfully with a
    # SKIP verdict unless the host is arm64 and executable memory is available,
    # so it is safe to register everywhere. Hosted CI additionally requires the
    # macOS runners to execute through MAP_JIT rather than skip or fall back.
    add_executable(test_jit_exec tests/test_jit_exec.c)
    target_link_libraries(test_jit_exec PRIVATE emucore)
    add_test(NAME jit_execution COMMAND test_jit_exec)
    # Deliberately do not set PASS_ or SKIP_REGULAR_EXPRESSION: either can
    # override a non-zero process result. The workflow separately validates
    # that macOS really executed MAP_JIT code instead of taking a skip path.
endif()

# --- unit tests -------------------------------------------------------------
add_executable(test_arm tests/test_arm.c)
target_link_libraries(test_arm PRIVATE emucore)
add_test(NAME arm_interp COMMAND test_arm)

add_executable(test_vfp tests/test_vfp.c)
target_link_libraries(test_vfp PRIVATE emucore)
add_test(NAME vfp COMMAND test_vfp)

add_executable(test_soc tests/test_soc.c)
target_link_libraries(test_soc PRIVATE emucore)
add_test(NAME s5l8900_machine COMMAND test_soc)

add_executable(test_i2c tests/test_i2c.c)
target_link_libraries(test_i2c PRIVATE emucore)
add_test(NAME s5l8900_i2c_pmu COMMAND test_i2c)

# The WM8991 codec and the two I2S windows. Separate from test_i2c because the
# property being defended is not a fact about the I2C controller: it is that a
# specific slave answers a specific identity, refuses a specific bit, and that
# the transport window behind it exists — the codec and the window are one
# milestone and are tested as one.
add_executable(test_wm8991 tests/test_wm8991.c)
target_link_libraries(test_wm8991 PRIVATE emucore)
add_test(NAME wm8991_codec_and_i2s COMMAND test_wm8991)

add_executable(test_spi tests/test_spi.c)
target_link_libraries(test_spi PRIVATE emucore)
add_test(NAME s5l8900_spi COMMAND test_spi)

add_executable(test_gpioic tests/test_gpioic.c)
target_link_libraries(test_gpioic PRIVATE emucore)
add_test(NAME s5l8900_gpioic COMMAND test_gpioic)

# The five physical buttons. Separate from test_gpioic because the property
# being defended is not a fact about the interrupt controller: it is that the
# board asserts NOTHING at rest and exactly one interrupt per transition once
# armed, and that spans the pin block, the controller's level rules and the
# device tree's own wiring at once.
add_executable(test_buttons tests/test_buttons.c)
target_link_libraries(test_buttons PRIVATE emucore)
add_test(NAME iphone1_2_buttons COMMAND test_buttons)

# uart4, the guest's PPP line. Separate from test_soc's general machine tests
# because the property being defended is a relationship between two devices --
# that the console capture and the PPP capture never become one buffer -- and
# that is not a fact about either UART on its own.
add_executable(test_uart4 tests/test_uart4.c)
target_link_libraries(test_uart4 PRIVATE emucore)
add_test(NAME s5l8900_uart4_ppp_line COMMAND test_uart4)

# The host PPP peer. Its centrepiece replays work/run80-ppp-tty/uart4-ppp.bin,
# the 47 bytes a real guest's pppd really transmitted; /work/ is git-ignored, so
# the test carries a transcription of those bytes and checks it against the file
# whenever the file is present. Passing the path as a definition rather than
# reading a relative name keeps the test independent of the working directory,
# exactly as test_bringup does with the firmware.
add_executable(test_ppp tests/test_ppp.c)
target_link_libraries(test_ppp PRIVATE emucore)
target_compile_definitions(test_ppp PRIVATE
    S5LBOX_RUN80_PPP="${CMAKE_CURRENT_SOURCE_DIR}/../work/run80-ppp-tty/uart4-ppp.bin")
add_test(NAME ppp_peer COMMAND test_ppp)

# The sockets behind net.c's egress table. Kept out of emucore for the same
# reason audio_capture.c and file_block.c are: core/ must link into the iOS app
# and run on a test runner with no network, so every socket call lives here.
add_library(nethost STATIC ../tools/net_host.c)
target_link_libraries(nethost PUBLIC emucore)
target_include_directories(nethost PUBLIC ../tools)
if(WIN32)
    target_link_libraries(nethost PUBLIC ws2_32)
endif()

add_executable(test_net tests/test_net.c)
target_link_libraries(test_net PRIVATE emucore)
add_test(NAME host_ipv4_nat COMMAND test_net)

add_executable(test_net_host tests/test_net_host.c)
target_link_libraries(test_net_host PRIVATE nethost)
add_test(NAME host_egress COMMAND test_net_host)


add_executable(test_mtz2 tests/test_mtz2.c)
target_link_libraries(test_mtz2 PRIVATE emucore)
add_test(NAME multitouch_z2 COMMAND test_mtz2)

# The two PL080 DMA controllers. Half of these tests drive the model through a
# fake bus, because that is the only way to assert the exact address, value AND
# WIDTH of every store; the other half go through a real machine, because "a DMA
# store lands where a CPU store lands" is a claim about machine.c's decode and
# cannot be checked against the device alone.
add_executable(test_pl080 tests/test_pl080.c)
target_link_libraries(test_pl080 PRIVATE emucore)
add_test(NAME s5l8900_pl080_dmac COMMAND test_pl080)

add_executable(test_img3 tests/test_img3.c)
target_link_libraries(test_img3 PRIVATE emucore)
add_test(NAME img3_parser COMMAND test_img3)

add_executable(test_aes tests/test_aes.c)
target_link_libraries(test_aes PRIVATE emucore)
add_test(NAME aes_fips197 COMMAND test_aes)

add_executable(test_loader tests/test_loader.c)
target_link_libraries(test_loader PRIVATE emucore)
add_test(NAME firmware_loader COMMAND test_loader)

add_executable(test_devicetree tests/test_devicetree.c)
target_link_libraries(test_devicetree PRIVATE emucore)
# tools/dt_inplace.h holds the same-length in-place writers bootkernel uses to
# stand in for iBoot. They decide values the guest cannot boot a display
# without, so they are tested here against the shipping implementation rather
# than against a copy of it.
target_include_directories(test_devicetree PRIVATE ../tools)
add_test(NAME device_tree COMMAND test_devicetree)

add_executable(test_lzss tests/test_lzss.c)
target_link_libraries(test_lzss PRIVATE emucore)
add_test(NAME lzss COMMAND test_lzss)

add_executable(test_ksyms tests/test_ksyms.c)
target_link_libraries(test_ksyms PRIVATE emucore)
add_test(NAME ksyms COMMAND test_ksyms)

add_executable(test_snapshot tests/test_snapshot.c)
target_link_libraries(test_snapshot PRIVATE emucore)
add_test(NAME snapshot COMMAND test_snapshot)

add_executable(test_nand tests/test_nand.c)
target_link_libraries(test_nand PRIVATE emucore)
add_test(NAME nand_device COMMAND test_nand)

add_executable(test_vm_source tests/test_vm_source.c)
target_link_libraries(test_vm_source PRIVATE emucore)
add_test(NAME vm_source COMMAND test_vm_source)

add_executable(test_vm_block tests/test_vm_block.c)
target_link_libraries(test_vm_block PRIVATE emucore)
add_test(NAME vm_block COMMAND test_vm_block)

add_executable(test_md_bridge tests/test_md_bridge.c)
target_link_libraries(test_md_bridge PRIVATE emucore)
add_test(NAME host_memory_disk_bridge COMMAND test_md_bridge)

add_executable(test_md_raw_bridge tests/test_md_raw_bridge.c)
target_link_libraries(test_md_raw_bridge PRIVATE emucore)
add_test(NAME host_raw_memory_disk_bridge COMMAND test_md_raw_bridge)

add_executable(test_macho tests/test_macho.c)
target_link_libraries(test_macho PRIVATE emucore)
add_test(NAME macho_parser COMMAND test_macho)

# VMGuest is the portable-C seam between the iOS shell and emucore.  Building
# it only with Xcode left its physical-address and scanout bounds checks outside
# the sanitizer suite, even though a mistake there becomes an out-of-bounds
# framebuffer read on the phone.  Keep this target host-only and tiny: it does
# not compile UIKit or allocate the real 128/512 MB guest configuration.
add_executable(test_vmguest
    ../app/Tests/test_vmguest.c
    ../app/Sources/VMGuest.c
)
target_include_directories(test_vmguest PRIVATE ../app/Sources)
target_link_libraries(test_vmguest PRIVATE emucore)
add_test(NAME app_vmguest_bridge COMMAND test_vmguest)

# The app's user interface is built by a macOS CI runner and can only be run on
# a phone, so none of its Objective-C is reachable from this suite.  The pieces
# that are pure arithmetic, pure data and pure bookkeeping are therefore
# deliberately NOT written in Objective-C, and are tested here instead: where a
# finger landed on the guest's panel, the option table the settings screen
# mirrors from tools/bootkernel.c, and which touch report gets dropped when the
# emulator thread cannot keep up.  Only the last of these needs a core header
# (for the contact struct it fills in); none of them links emucore, because none
# of them has anything to do with a running machine.
# The app's firmware boot path. Objective-C cannot be compiled by this runner,
# so everything between "there are files in a directory" and "the machine is
# pointed at Apple's kernel" is plain C in app/Sources/VMFirmwareBoot.c and is
# tested here. What it protects is narrow and important: that the app cannot
# report a firmware boot it did not perform. Needs no firmware -- every fixture
# is a file the test writes itself.
add_executable(test_vmfirmwareboot
    ../app/Tests/test_vmfirmwareboot.c
    ../app/Sources/VMFirmwareBoot.c
    ../app/Sources/VMBootOptions.c
    ../app/Sources/VMOptions.c
)
target_include_directories(test_vmfirmwareboot PRIVATE ../app/Sources ../tools)
target_link_libraries(test_vmfirmwareboot PRIVATE
    emucore host_ios3_bringup_gate host_file_block host_rootfs_work)
set_target_properties(test_vmfirmwareboot PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED YES
    C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_vmfirmwareboot PRIVATE /W4 /WX)
else()
    target_compile_options(test_vmfirmwareboot PRIVATE
        -Wall -Wextra -Werror -pedantic -Wformat=2 -Wshadow)
endif()
target_compile_definitions(test_vmfirmwareboot PRIVATE
    S5LBOX_FIRMWARE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../firmware")
add_test(NAME app_firmware_boot COMMAND test_vmfirmwareboot)
set_tests_properties(app_firmware_boot PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")

# Which of the settings screen's fourteen switches reaches a bring-up request,
# and what the app says about the ones that do not.  Two do; twelve do not; on
# an untouched installation six are already in conflict with what bring-up
# actually does, and this is the only place that can be asserted without a
# phone.  Needs no core symbols -- s5l_bringup_request_t is a struct and
# nothing here calls into emucore -- so it does not link it.
add_executable(test_vmbootoptions
    ../app/Tests/test_vmbootoptions.c
    ../app/Sources/VMBootOptions.c
    ../app/Sources/VMOptions.c
)
target_include_directories(test_vmbootoptions PRIVATE ../app/Sources include)
set_target_properties(test_vmbootoptions PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED YES
    C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_vmbootoptions PRIVATE /W4 /WX)
else()
    target_compile_options(test_vmbootoptions PRIVATE
        -Wall -Wextra -Werror -pedantic -Wformat=2 -Wshadow)
endif()
add_test(NAME app_boot_options COMMAND test_vmbootoptions)

# Whether a device will branch into a page this process just wrote. Every
# throughput number in docs/dynarec.md §10.3 is conditional on that answer and
# it has never been measured -- `RWX mmap: YES` from a user's stock iOS 26.1
# phone proves the MAPPING was granted, not that executing it is permitted.
#
# The test cannot assert the policy without becoming a test of whichever host
# ran it; it asserts that every strategy is SURVIVABLE and self-consistent,
# which is the property that lets this ship to a phone. The bytes are really
# executed on the core-tests matrix: x86-64 Linux takes the x86 payload and
# arm64 macOS the aarch64 one. Links no core symbols.
#
# A WARNING FOR ANYONE EDITING VMJitProbe.c FROM WINDOWS. There is no mmap on
# MinGW, so VM_JIT_POSIX is 0 and the ENTIRE mechanism -- the mapping, the
# signal handlers, the call -- is preprocessed away. A green local run says
# nothing about that code. The first push of this file passed 50/50 here and
# failed CI three ways: a pedantic object-to-function pointer cast, an unused
# variable where the MAP_JIT blocks compile out, and
# pthread_jit_write_protect_np being unavailable on iOS. Push and read CI;
# local green is not evidence for this one file.
add_executable(test_vmjitprobe
    ../app/Tests/test_vmjitprobe.c
    ../app/Sources/VMJitProbe.c
)
target_include_directories(test_vmjitprobe PRIVATE ../app/Sources)
set_target_properties(test_vmjitprobe PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED YES
    C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_vmjitprobe PRIVATE /W4 /WX)
else()
    target_compile_options(test_vmjitprobe PRIVATE
        -Wall -Wextra -Werror -pedantic -Wformat=2 -Wshadow)
endif()
add_test(NAME app_jit_probe COMMAND test_vmjitprobe)

# Guest-function HLE (tools/ios3_hle.c). The composited-pixel work the guest
# does only because this VM un-matches the GPU is the one cost no interpreter
# speed absorbs, and intercepting a leaf blit is how it stops being executed.
# It needs no JIT: the replacement is compiled C in this binary's signed
# __TEXT and the interception is a PC compare, so nothing is generated at
# runtime and there is nothing for iOS to refuse.
#
# The tests are almost all REFUSALS -- no prologue, wrong bytes, unreadable
# bytes, wrong address space, handler declines -- because a facility that
# replaces functions inside a running OS is dangerous in proportion to how
# willing it is to fire. Whether a replacement computes the same PIXELS cannot
# be unit-tested; that needs a frame diff of a real run armed and disarmed,
# which is the oracle named in the header's contract.
add_executable(test_ios3_hle
    tests/test_ios3_hle.c
    ../tools/ios3_hle.c
)
target_include_directories(test_ios3_hle PRIVATE ../tools include)
set_target_properties(test_ios3_hle PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED YES
    C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_ios3_hle PRIVATE /W4 /WX)
else()
    target_compile_options(test_ios3_hle PRIVATE
        -Wall -Wextra -Werror -pedantic -Wformat=2 -Wshadow)
endif()
add_test(NAME ios3_hle COMMAND test_ios3_hle)

# Which files belong to which machine.  The identifier from the machine list
# becomes a PATH COMPONENT here, so the check that admits only sixteen
# lower-case hex digits is a security boundary and not a formatting rule; and
# the adoption of a pre-instance work image is the one operation in the app
# that can lose 450 MB of somebody's guest disk.  Links the firmware boot path
# because the property worth asserting is end to end: that two machines see two
# root filesystems through the code that actually opens them.
add_executable(test_vminstancepaths
    ../app/Tests/test_vminstancepaths.c
    ../app/Sources/VMInstancePaths.c
    ../app/Sources/VMInstances.c
    ../app/Sources/VMFirmwareBoot.c
    ../app/Sources/VMBootOptions.c
    ../app/Sources/VMOptions.c
)
target_include_directories(test_vminstancepaths PRIVATE ../app/Sources ../tools)
target_link_libraries(test_vminstancepaths PRIVATE
    emucore host_ios3_bringup_gate host_file_block host_rootfs_work)
set_target_properties(test_vminstancepaths PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED YES
    C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_vminstancepaths PRIVATE /W4 /WX)
else()
    target_compile_options(test_vminstancepaths PRIVATE
        -Wall -Wextra -Werror -pedantic -Wformat=2 -Wshadow)
endif()
add_test(NAME app_instance_paths COMMAND test_vminstancepaths)
set_tests_properties(app_instance_paths PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")

add_executable(test_vmtouchmap
    ../app/Tests/test_vmtouchmap.c
    ../app/Sources/VMTouchMap.c
)
target_include_directories(test_vmtouchmap PRIVATE ../app/Sources)
# floor() and isfinite(): the same libm split that caught emucore on Linux.
if(NOT WIN32 AND NOT APPLE)
    target_link_libraries(test_vmtouchmap PRIVATE m)
endif()
add_test(NAME app_touch_map COMMAND test_vmtouchmap)

add_executable(test_vmtouchqueue
    ../app/Tests/test_vmtouchqueue.c
    ../app/Sources/VMTouchQueue.c
)
target_include_directories(test_vmtouchqueue PRIVATE ../app/Sources include)
add_test(NAME app_touch_queue COMMAND test_vmtouchqueue)

# The app's button queue and, more importantly, its mapping onto the core's
# buttons. The app's enum is in the order the control bar draws (Home first) and
# the core's is /device-tree/buttons' own (hold first); a cast between them
# compiles, runs, and quietly sends Power when the user pressed Home. Links
# buttons.c so the names and identifiers are checked against the model itself
# rather than against a copy of it.
add_executable(test_vmbuttonqueue
    ../app/Tests/test_vmbuttonqueue.c
    ../app/Sources/VMButtonQueue.c
    src/soc/buttons.c
    src/soc/gpioic.c   # buttons.c drives both halves of /arm-io/gpio
)
target_include_directories(test_vmbuttonqueue PRIVATE ../app/Sources include)
add_test(NAME app_button_queue COMMAND test_vmbuttonqueue)

# The machine list. Links VMOptions too, because the one bound that protects
# every saved machine -- that an instance can still store a value for every
# option row -- is a fact about both tables at once.
add_executable(test_vminstances
    ../app/Tests/test_vminstances.c
    ../app/Sources/VMInstances.c
    ../app/Sources/VMOptions.c
)
target_include_directories(test_vminstances PRIVATE ../app/Sources)
add_test(NAME app_instance_list COMMAND test_vminstances)

add_executable(test_vmoptions
    ../app/Tests/test_vmoptions.c
    ../app/Sources/VMOptions.c
)
target_include_directories(test_vmoptions PRIVATE ../app/Sources)
add_test(NAME app_option_table COMMAND test_vmoptions)

# The IPSW importer. Every byte it reads comes from a file the user found on the
# internet, so this is the app's largest untrusted-input surface by a wide
# margin -- a zip central directory, a DEFLATE stream, an XML property list, an
# IMG3 container, an AES-wrapped disk image and a table of compressed chunks,
# all of them carrying lengths and offsets nobody has checked.
#
# It is therefore written in plain C rather than Objective-C, for the same
# reason as VMTouchMap and VMOptions: so a host runner can test it. It links
# emucore for the IMG3, AES and LZSS implementations that already exist and are
# already pinned by test_img3/test_aes/test_lzss -- the importer reuses those
# rather than carrying a second copy of the byte-level decisions -- and
# host_sha256 for the digest that decides whether an artefact is the right one.
# The app's IPSW importer, driven from a terminal. Its 1,126 assertions all run
# against fixtures this project built, which proves the parser does what it was
# written to do and says nothing about whether it can open Apple's actual
# archive -- a different claim, and the one a user is really asking about.
# Identify-only by default: no output paths, so it opens nothing for writing
# and needs no key.
add_executable(fwimport
    ../tools/fwimport.c
    ../app/Sources/VMFirmwareInflate.c
    ../app/Sources/VMFirmwareDigest.c
    ../app/Sources/VMFirmwarePlist.c
    ../app/Sources/VMFirmwareZip.c
    ../app/Sources/VMFirmwareDMG.c
    ../app/Sources/VMFirmwareImport.c
)
target_include_directories(fwimport PRIVATE ../app/Sources include ../tools)
target_link_libraries(fwimport PRIVATE emucore host_sha256)
set_target_properties(fwimport PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED YES
    C_EXTENSIONS NO)

add_executable(test_vmfirmwareimport
    ../app/Tests/test_vmfirmwareimport.c
    ../app/Tests/test_vmfw_inflate.c
    ../app/Tests/test_vmfw_digest.c
    ../app/Tests/test_vmfw_plist.c
    ../app/Tests/test_vmfw_zip.c
    ../app/Tests/test_vmfw_dmg.c
    ../app/Tests/test_vmfw_import.c
    ../app/Sources/VMFirmwareInflate.c
    ../app/Sources/VMFirmwareDigest.c
    ../app/Sources/VMFirmwarePlist.c
    ../app/Sources/VMFirmwareZip.c
    ../app/Sources/VMFirmwareDMG.c
    ../app/Sources/VMFirmwareImport.c
)
target_include_directories(test_vmfirmwareimport
    PRIVATE ../app/Sources ../app/Tests include ../tools)
target_link_libraries(test_vmfirmwareimport PRIVATE emucore host_sha256)
set_target_properties(test_vmfirmwareimport PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED YES
    C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_vmfirmwareimport PRIVATE /W4 /WX)
else()
    target_compile_options(test_vmfirmwareimport PRIVATE
        -Wall -Wextra -Werror -Wformat=2 -Wshadow)
endif()
add_test(NAME app_firmware_import COMMAND test_vmfirmwareimport)

# --- host tools -------------------------------------------------------------
# The host-owned writable-file adapter stays outside emucore: the portable core
# exposes vm_block_t callbacks but never depends on descriptors, locks, or
# platform sync APIs.  Build this small boundary with the strictest warnings.
add_library(host_file_block STATIC ../tools/file_block.c)
target_include_directories(host_file_block PUBLIC ../tools include)
set_target_properties(host_file_block PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED YES
    C_EXTENSIONS NO)
if(NOT WIN32)
    target_compile_definitions(host_file_block PRIVATE _FILE_OFFSET_BITS=64)
endif()
if(MSVC)
    target_compile_options(host_file_block PRIVATE /W4 /WX)
else()
    target_compile_options(host_file_block PRIVATE
        -Wall -Wextra -Werror -pedantic)
endif()

add_executable(test_file_block tests/test_file_block.c)
target_link_libraries(test_file_block PRIVATE emucore host_file_block)
set_target_properties(test_file_block PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED YES
    C_EXTENSIONS NO)
if(MSVC)
    # Fixture helpers deliberately use ISO fopen; contain the secure-CRT opt-out
    # to this test instead of weakening warnings for production targets.
    target_compile_definitions(test_file_block PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(test_file_block PRIVATE /W4 /WX)
else()
    target_compile_options(test_file_block PRIVATE
        -Wall -Wextra -Werror -pedantic)
endif()
add_test(NAME file_block_adapter COMMAND test_file_block)
set_tests_properties(file_block_adapter PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")

# Kernel compatibility patches are a host policy, not a property of the CPU
# core.  Keep their bounded, firmware-neutral transaction helper outside
# emucore so a frontend must opt into a version-specific manifest explicitly.
add_library(host_guest_patch STATIC ../tools/guest_patch.c)
target_include_directories(host_guest_patch PUBLIC ../tools)
set_target_properties(host_guest_patch PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED YES
    C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(host_guest_patch PRIVATE /W4 /WX)
else()
    target_compile_options(host_guest_patch PRIVATE
        -Wall -Wextra -Werror -pedantic)
endif()

add_executable(test_guest_patch tests/test_guest_patch.c)
target_link_libraries(test_guest_patch PRIVATE host_guest_patch)
set_target_properties(test_guest_patch PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED YES
    C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_guest_patch PRIVATE /W4 /WX)
else()
    target_compile_options(test_guest_patch PRIVATE
        -Wall -Wextra -Werror -pedantic)
endif()
add_test(NAME atomic_guest_patch COMMAND test_guest_patch)

# The drag gesture the harness schedules: the phase sequence, the identifier and
# the interpolated path, as pure arithmetic over emucore's contact struct.  Kept
# out of tools/bootkernel.c for exactly the reason above -- it is a host policy,
# not a property of the device -- and, more usefully, because that is what lets
# it be pinned in milliseconds.  run90 spent a full boot discovering that eight
# taps on one path identifier are eight fingers to UIKit; the tests here would
# have cost nothing.
add_library(host_mt_drag STATIC ../tools/mt_drag.c)
target_include_directories(host_mt_drag PUBLIC ../tools)
target_link_libraries(host_mt_drag PUBLIC emucore)   # s5l_mt_contact_t, phases
set_target_properties(host_mt_drag PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED YES
    C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(host_mt_drag PRIVATE /W4 /WX)
else()
    target_compile_options(host_mt_drag PRIVATE
        -Wall -Wextra -Werror -pedantic)
endif()

add_executable(test_mt_drag tests/test_mt_drag.c)
target_link_libraries(test_mt_drag PRIVATE host_mt_drag emucore)
add_test(NAME mt_drag_gesture COMMAND test_mt_drag)

# Writing the guest's I2S transmit stream to a file is host filesystem work,
# not emulated machine behaviour -- the same argument that keeps file_block.c
# and rootfs_work.c out of emucore, and the same one that put the framebuffer's
# PPM capture in tools/bootkernel.c rather than in clcd.c. It links emucore for
# the device-tree parser it asks about the sample rate and for the I2S window's
# own FIFO classifier, so the format question is answered against the shipping
# implementations rather than against a copy of them.
add_library(host_audio_capture STATIC ../tools/audio_capture.c)
target_include_directories(host_audio_capture PUBLIC ../tools include)
target_link_libraries(host_audio_capture PUBLIC emucore)
set_target_properties(host_audio_capture PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED YES
    C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(host_audio_capture PRIVATE /W4 /WX)
    # This target opens a file, and MSVC's C4996 calls fopen unsafe and /WX
    # turns that into an error. fopen_s is not an option: this is C11 built for
    # four toolchains and the sink must behave identically on all of them.
    # test_file_block already carries the same definition for the same reason.
    target_compile_definitions(host_audio_capture PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(host_audio_capture PRIVATE
        -Wall -Wextra -Werror -pedantic)
endif()

# The suite runs in the build directory because several cases turn on a file
# NOT existing, and a fixed relative name resolved against an unknown working
# directory could find somebody else's. The firmware directory is passed as a
# definition so the one case that reads the real 7E18 tree is independent of
# where it is run from, exactly as test_bringup does; that case skips when the
# firmware is absent, which is the state of every public CI runner.
add_executable(test_audio_capture tests/test_audio_capture.c)
target_link_libraries(test_audio_capture PRIVATE host_audio_capture emucore)
target_compile_definitions(test_audio_capture PRIVATE
    S5LBOX_FIRMWARE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../firmware")
add_test(NAME guest_audio_capture COMMAND test_audio_capture)
set_tests_properties(guest_audio_capture PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")

# The exact kernel and rootfs policies authenticate caller-selected bytes with
# the same small, allocation-free implementation.  Keep that primitive in a
# separate strict target so neither host policy carries a private hash copy.
add_library(host_sha256 STATIC ../tools/sha256.c)
target_include_directories(host_sha256 PUBLIC ../tools)
set_target_properties(host_sha256 PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED YES
    C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(host_sha256 PRIVATE /W4 /WX)
else()
    target_compile_options(host_sha256 PRIVATE
        -Wall -Wextra -Werror -pedantic)
endif()

# The exact iPhone OS 3.1.3 kernel manifest is a second host-policy layer over
# the firmware-neutral transaction helper.  Keeping it in its own target makes
# the supported build identity explicit and prevents the portable CPU core from
# silently accepting version-specific patches.
add_library(host_ios3_kernel_patch STATIC ../tools/ios3_kernel_patch.c)
target_include_directories(host_ios3_kernel_patch PUBLIC ../tools include)
target_link_libraries(host_ios3_kernel_patch
    PUBLIC host_guest_patch host_sha256
    PRIVATE emucore)
set_target_properties(host_ios3_kernel_patch PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED YES
    C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(host_ios3_kernel_patch PRIVATE /W4 /WX)
else()
    target_compile_options(host_ios3_kernel_patch PRIVATE
        -Wall -Wextra -Werror -pedantic)
endif()

# The gate as a bring-up hook: one adapter, shared by the desktop test below
# and by the iOS app, so a kernel is authorized by the same code in both.
add_library(host_ios3_bringup_gate STATIC ../tools/ios3_bringup_gate.c)
target_include_directories(host_ios3_bringup_gate PUBLIC ../tools include)
target_link_libraries(host_ios3_bringup_gate
    PUBLIC host_ios3_kernel_patch emucore)
set_target_properties(host_ios3_bringup_gate PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED YES
    C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(host_ios3_bringup_gate PRIVATE /W4 /WX)
else()
    target_compile_options(host_ios3_bringup_gate PRIVATE
        -Wall -Wextra -Werror -pedantic)
endif()

# The orchestration that turns three files into a running kernel, against the
# real firmware. The layout it computes is the one ~90 recorded desktop runs
# were made with, so this test compares against those printed addresses rather
# than against itself.
#
# Public CI has no Apple firmware, so the firmware-backed cases skip there and
# the fail-closed refusal cases -- the ones that stop the app claiming a boot
# that did not happen -- run everywhere. ../tools is on the include path for
# dt_inplace.h, which the test uses to read the patched tree back through the
# shipping implementation instead of through a copy of bringup.c's writers.
add_executable(test_bringup tests/test_bringup.c)
target_link_libraries(test_bringup PRIVATE emucore host_ios3_bringup_gate)
target_include_directories(test_bringup PRIVATE ../tools)
target_compile_definitions(test_bringup PRIVATE
    S5LBOX_FIRMWARE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../firmware")
set_target_properties(test_bringup PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED YES
    C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_bringup PRIVATE /W4 /WX)
else()
    target_compile_options(test_bringup PRIVATE
        -Wall -Wextra -Werror -pedantic)
endif()
add_test(NAME kernel_bringup COMMAND test_bringup)

add_executable(test_ios3_kernel_patch tests/test_ios3_kernel_patch.c)
target_link_libraries(test_ios3_kernel_patch PRIVATE host_ios3_kernel_patch)
set_target_properties(test_ios3_kernel_patch PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED YES
    C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_ios3_kernel_patch PRIVATE /W4 /WX)
else()
    target_compile_options(test_ios3_kernel_patch PRIVATE
        -Wall -Wextra -Werror -pedantic)
endif()
add_test(NAME exact_ios3_kernel_patch COMMAND test_ios3_kernel_patch)

# Provisioning a writable rootfs image is host filesystem work, not emulated
# machine behavior.  The implementation is intentionally standalone and uses
# at most a 1 MiB transfer buffer, so its format and publication tests remain
# cheap on every hosted runner.
add_library(host_rootfs_work STATIC ../tools/rootfs_work.c)
target_include_directories(host_rootfs_work PUBLIC ../tools)
target_link_libraries(host_rootfs_work PUBLIC host_sha256)
set_target_properties(host_rootfs_work PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED YES
    C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(host_rootfs_work PRIVATE /W4 /WX)
else()
    target_compile_options(host_rootfs_work PRIVATE
        -Wall -Wextra -Werror -pedantic)
endif()

add_executable(test_rootfs_work tests/test_rootfs_work.c)
target_link_libraries(test_rootfs_work PRIVATE host_rootfs_work)
set_target_properties(test_rootfs_work PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED YES
    C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_rootfs_work PRIVATE /W4 /WX)
else()
    target_compile_options(test_rootfs_work PRIVATE
        -Wall -Wextra -Werror -pedantic)
endif()
add_test(NAME bounded_rootfs_work_image COMMAND test_rootfs_work)
set_tests_properties(bounded_rootfs_work_image PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")

# The catalog B-tree writer is the part of rootfs_work.c that a guest kernel
# mounts and trusts, so it gets its own suite. Its fixtures are whole HFSX
# volumes built in memory — B-tree header node, leaf and index nodes,
# allocation bitmap, matching alternate volume header — which keeps the suite
# firmware-free and runnable on every public CI runner.
add_executable(test_rootfs_provision tests/test_rootfs_provision.c)
target_link_libraries(test_rootfs_provision PRIVATE host_rootfs_work)
set_target_properties(test_rootfs_provision PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED YES
    C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_rootfs_provision PRIVATE /W4 /WX)
else()
    target_compile_options(test_rootfs_provision PRIVATE
        -Wall -Wextra -Werror -pedantic)
endif()
add_test(NAME hfs_catalog_provisioning COMMAND test_rootfs_provision)
set_tests_properties(hfs_catalog_provisioning PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")

# img3dump points the real parser at real firmware, so inspecting an IPSW also
# validates our format handling.
add_executable(img3dump ../tools/img3dump.c)
target_link_libraries(img3dump PRIVATE emucore)

# runfw executes a decrypted payload on the emulated machine and reports exactly
# where and why it stopped — the diagnostic that says what to build next.
add_executable(runfw ../tools/runfw.c)
target_link_libraries(runfw PRIVATE emucore)

add_executable(unlzss ../tools/unlzss.c)
target_link_libraries(unlzss PRIVATE emucore)

add_executable(machoinfo ../tools/machoinfo.c)
target_link_libraries(machoinfo PRIVATE emucore)

add_executable(bootkernel ../tools/bootkernel.c)
target_link_libraries(bootkernel PRIVATE
    emucore host_audio_capture host_file_block host_guest_patch
    host_ios3_kernel_patch host_mt_drag host_rootfs_work nethost)

# Public CI cannot contain Apple's firmware, but it can still exercise the
# external-md parser's fail-closed boundary before any file is opened. These
# cases protect the combinations that would otherwise mix incompatible storage
# or checkpoint ownership models.
add_test(NAME external_md_cli_preflight
    COMMAND ${CMAKE_COMMAND}
            -DBOOTKERNEL=$<TARGET_FILE:bootkernel>
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_bootkernel_external_md.cmake)

# The same trick applied to the option layer itself. --print-config resolves and
# validates a whole command line and exits 0 without opening the kernel, the
# tree or any image, so the resolver's defaults, its --no- overrides, its
# short-flag aliases and its fail-closed rejections can all be pinned without
# Apple firmware. These are the cases that stop a default drifting silently and
# retroactively changing what ~60 runs in docs/BOOTLOG.md mean.
add_test(NAME boot_option_resolution
    COMMAND ${CMAKE_COMMAND}
            -DBOOTKERNEL=$<TARGET_FILE:bootkernel>
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_bootkernel_options.cmake)

# The app mirrors that table by hand, and its own test cannot notice a row
# bootkernel GAINED -- it compares the app's copy against the app's own
# expectation, so both can be edited together and still be short a toggle.
# This runs both binaries and requires bootkernel's live table to partition
# into the mirrored rows and the deliberately-omitted ones.
# app/Info.plist carried an illegal XML comment for an unknown length of time --
# XML forbids "--" inside one, and a comment used it as a dash. Nothing read the
# file, and a malformed plist fails at build or launch, a long way from the
# punctuation. Narrow on purpose: plutil -lint on a macOS runner is the real
# validator; this is the part that runs wherever CTest does.
add_test(NAME app_info_plist
    COMMAND ${CMAKE_COMMAND}
            -DPLIST=${CMAKE_CURRENT_SOURCE_DIR}/../app/Info.plist
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_info_plist.cmake)

add_test(NAME app_option_mirror
    COMMAND ${CMAKE_COMMAND}
            -DBOOTKERNEL=$<TARGET_FILE:bootkernel>
            -DVMOPTIONS=$<TARGET_FILE:test_vmoptions>
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_option_mirror.cmake)

# snapboot is the snapshot acceptance harness: it boots to a checkpoint, saves,
# and can restore into a fresh process. Comparing snapshot bytes alone is not
# enough — a field the format never stores is absent from both sides and
# cancels out — so it also prints a machine-derived report to diff.
add_executable(snapboot ../tools/snapboot.c)
target_link_libraries(snapboot PRIVATE emucore)

# insnbench measures interpreter throughput on the six synthetic configurations
# docs/dynarec.md §1.1-§1.2 used, whose every recorded number was taken on one
# desktop x86-64 box. Built here rather than as a test on purpose: it produces a
# rate, not a verdict, so it is deliberately NOT registered with ctest and must
# not become a performance gate — a threshold measured on a shared, virtualised
# CI runner would either catch nothing or flake. .github/workflows/core-tests.yml
# runs it as a non-gating step in the existing three-platform matrix, which is
# what makes the arm64-vs-x86 ratio a same-commit, same-day comparison instead of
# a comparison against historical numbers from a different tree.
#
# $<CONFIG> is a single bare token in every generator, so passing it as a
# definition cannot go wrong the way an embedded flags string with spaces could.
# The workflow prints the resolved compiler and CMAKE_C_FLAGS_<CONFIG> out of the
# cache alongside the run, which is where the literal -O level comes from.
add_executable(insnbench ../tools/insnbench.c)
target_link_libraries(insnbench PRIVATE emucore)
target_compile_definitions(insnbench PRIVATE INSNBENCH_BUILD_TYPE="$<CONFIG>")
